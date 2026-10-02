// Build 35924: docs/re/linux/TRAIN_ORDER.md. No Windows layouts or ABI here.
#include "train_order_linux.h"
#include "slice_core.h"
#include "near_alloc.h"
#include "train_order_checks.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include "trainorder.h"
#include "flags_linux.h"

namespace {
uintptr_t gameBase;
bool installed = false;
std::atomic<uint64_t> steps{0}, reorders{0}, refusals{0}, maxUs{0};
std::atomic<uint32_t> lastSeedValue{0};
std::atomic<int64_t> lastCount{-1};
using Shuffle = void (*)(int32_t*, int32_t*, uint64_t*);
using SeedGetter = uint32_t (*)(void*);
using TypeFind = uintptr_t (*)(void*, const uintptr_t*);

// Missing names are normal. Never call the engine's asserting slot lookup.
uintptr_t NameComponent(uintptr_t world, int32_t id, int type)
{
    if (id < 0 || type < 0) return 0;
    uintptr_t entities = 0, pools = 0, pool = 0;
    if (!SliceReadT(world + 0x98, &entities) || !entities ||
        !SliceReadT(world + 0x80, &pools) || !pools ||
        !SliceReadT(pools + size_t(type) * 8, &pool) || !pool) return 0;
    SliceVec slots{};
    if (!SliceReadStdVectorShape(entities + size_t(id) * 24, 8, 4096, &slots)) return 0;
    // All pairs in one guarded read, not one per pair (every road Add names each
    // vehicle on the edge; 2026-09-27).
    static thread_local int32_t pairs[4096][2];
    if (slots.count && !SliceRead(slots.begin, pairs, slots.count * 8)) return 0;
    for (size_t i = 0; i < slots.count; ++i) {
        const int32_t* pair = pairs[i];
        if (pair[0] != type) continue;
        if (pair[1] < 0) return 0;
        uintptr_t data = 0;
        if (pair[1] < 0x40000000) {
            if (!SliceReadT(pool + 0xb8, &data) || !data) return 0;
            return data + size_t(pair[1]) * 32;
        }
        const uint32_t slot = uint32_t(pair[1]) - 0x40000000u;
        uintptr_t pages = 0;
        if (!SliceReadT(pool + 0xd0, &pages) || !pages ||
            !SliceReadT(pages + size_t(slot / 32) * 16, &data) || !data) return 0;
        return data + size_t(slot % 32) * 32;
    }
    return 0;
}

} // namespace
uintptr_t SliceNameComponent(uintptr_t world, int32_t id, int type)
{ return NameComponent(world, id, type); }

// NameComponent + SliceReadStdString(.., TRAINORDER_NAME_MAX) for every id, a level
// at a time: each level is one SliceReadMany, so an edge of n vehicles costs a few
// syscalls instead of ~8n (the road sort was 13% of the server's sim thread,
// 2026-09-27). The world's pointers are read once; they do not move during the
// engine call this runs inside. A name is empty exactly where the one-at-a-time
// path fails.
void SliceEntityNames(uintptr_t world, int type, const int32_t* ids, size_t n, std::vector<std::string>* names)
{
    names->resize(n);
    for (auto& s : *names) s.clear();
    if (!world || type < 0 || !n) return;
    uintptr_t entities = 0, pools = 0, pool = 0;
    if (!SliceReadT(world + 0x98, &entities) || !entities ||
        !SliceReadT(world + 0x80, &pools) || !pools ||
        !SliceReadT(pools + size_t(type) * 8, &pool) || !pool) return;
    thread_local std::vector<SliceReadItem> items;
    thread_local std::vector<uint64_t> headers;      // 3 words per entry
    thread_local std::vector<SliceVec> slots;
    thread_local std::vector<size_t> pairAt;          // entry -> first pair in `pairs`
    thread_local std::vector<int32_t> pairs;
    thread_local std::vector<uintptr_t> comp;
    thread_local std::vector<char> text;              // TRAINORDER_NAME_MAX + 1 per entry
    items.resize(n); headers.assign(n * 3, 0); slots.assign(n, SliceVec{}); comp.assign(n, 0);
    // 1. each entity's slot vector header
    for (size_t i = 0; i < n; ++i)
        items[i] = { ids[i] < 0 ? 0 : entities + size_t(ids[i]) * 24, &headers[i * 3], ids[i] < 0 ? 0u : 24u, false };
    SliceReadMany(items.data(), n);
    size_t total = 0;
    pairAt.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (ids[i] < 0 || !items[i].ok ||
            !SliceStdVectorFromHeader(items[i].addr, &headers[i * 3], 8, 4096, &slots[i])) { slots[i] = {}; continue; }
        pairAt[i] = total; total += slots[i].count * 2;
    }
    // 2. their {type, slot} pairs
    pairs.assign(total, 0);
    for (size_t i = 0; i < n; ++i)
        items[i] = { slots[i].begin, pairs.data() + pairAt[i], slots[i].count * 8, false };
    SliceReadMany(items.data(), n);
    uintptr_t data = 0, pages = 0;
    bool dataRead = false, dataOk = false, pagesRead = false, pagesOk = false;
    thread_local std::vector<uint32_t> paged;         // entries whose data pointer is on a page
    paged.clear();
    for (size_t i = 0; i < n; ++i) {
        if (!slots[i].count || !items[i].ok) continue;
        for (size_t k = 0; k < slots[i].count; ++k) {
            const int32_t* pair = &pairs[pairAt[i] + k * 2];
            if (pair[0] != type) continue;
            if (pair[1] < 0) break;
            if (pair[1] < 0x40000000) {
                if (!dataRead) { dataRead = true; dataOk = SliceReadT(pool + 0xb8, &data) && data; }
                if (dataOk) comp[i] = data + size_t(pair[1]) * 32;
            } else {
                if (!pagesRead) { pagesRead = true; pagesOk = SliceReadT(pool + 0xd0, &pages) && pages; }
                if (pagesOk) { comp[i] = uint32_t(pair[1]) - 0x40000000u; paged.push_back(uint32_t(i)); }
            }
            break;
        }
    }
    // 2b. the page each paged entry's data sits on
    if (!paged.empty()) {
        thread_local std::vector<uintptr_t> pageData;
        pageData.assign(paged.size(), 0);
        for (size_t j = 0; j < paged.size(); ++j)
            items[j] = { pages + size_t(comp[paged[j]] / 32) * 16, &pageData[j], 8, false };
        SliceReadMany(items.data(), paged.size());
        for (size_t j = 0; j < paged.size(); ++j) {
            const size_t i = paged[j];
            comp[i] = items[j].ok && pageData[j] ? pageData[j] + size_t(comp[i] % 32) * 32 : 0;
        }
    }
    // 3. the std::string headers {p, len, capacity}
    headers.assign(n * 3, 0);
    for (size_t i = 0; i < n; ++i) items[i] = { comp[i], &headers[i * 3], comp[i] ? 24u : 0u, false };
    SliceReadMany(items.data(), n);
    // 4. the bytes, with their terminator
    const size_t cap = TRAINORDER_NAME_MAX + 1;
    if (text.size() < n * cap) text.resize(n * cap);   // each read brings its own terminator
    for (size_t i = 0; i < n; ++i) {
        const uintptr_t obj = comp[i], p = headers[i * 3];
        const size_t len = size_t(headers[i * 3 + 1]);
        bool shape = obj && items[i].ok && len <= TRAINORDER_NAME_MAX && len < cap;
        if (shape) shape = p == obj + 16 ? len <= 15 : (p && headers[i * 3 + 2] >= len);
        items[i] = { shape ? p : 0, &text[i * cap], shape ? len + 1 : 0, false };
    }
    SliceReadMany(items.data(), n);
    for (size_t i = 0; i < n; ++i) {
        if (!items[i].n || !items[i].ok) continue;
        const size_t len = items[i].n - 1;
        if (text[i * cap + len] == 0) (*names)[i].assign(&text[i * cap], len);
    }
}
namespace {

// All allocation and sorting is private. Refusal leaves the engine array
// untouched, so the original shuffle remains a valid fallback. Copy names to
// avoid dereferencing live engine strings from a comparator.
bool Arrange(int32_t* begin, int32_t* end, uintptr_t self, uintptr_t world,
             int type, uint32_t seed) noexcept
{
    const uintptr_t b = reinterpret_cast<uintptr_t>(begin), e = reinterpret_cast<uintptr_t>(end);
    if (e < b || (e - b) % 4 || (e - b) / 4 > TRAINORDER_MAX_N) return false;
    const size_t n = (e - b) / 4;
    if (n < 2) return true;
    uintptr_t holder = 0;
    SliceVec records{};
    if (!SliceReadT(self + 8, &holder) || !holder ||
        !SliceReadStdVector(holder, TRAINORDER_REC, TRAINORDER_MAX_N, &records) ||
        records.count != n) return false;
    try {
        thread_local std::vector<int32_t> order;
        thread_local std::vector<uint8_t> recs;
        thread_local std::vector<TrainOrderKey> keys;
        thread_local std::vector<std::string> names;
        order.resize(n); recs.resize(n * TRAINORDER_REC); keys.resize(n); names.resize(n);
        if (!SliceRead(b, order.data(), n * 4) ||
            !SliceRead(records.begin, recs.data(), recs.size())) return false;
        for (size_t i = 0; i < n; ++i) {
            const int32_t id = TrainOrderRecId(recs.data(), int32_t(i));
            char text[TRAINORDER_NAME_MAX + 1];
            size_t len = 0;
            const uintptr_t component = NameComponent(world, id, type);
            if (component && SliceReadStdString(component, text, sizeof(text), &len, TRAINORDER_NAME_MAX))
                names[i].assign(text, len);
            else names[i].clear();
            keys[i] = {names[i].data(), uint32_t(names[i].size()), id, 0};
        }
        const auto outcome = TrainOrderArrange(order.data(), int64_t(n), keys.data(), seed);
        if (outcome.refused) return false;
        // This is the engine's writable allocation, still owned by Update2.
        std::memcpy(begin, order.data(), n * 4);
        if (outcome.changed) ++reorders;
        static thread_local size_t lastN = SIZE_MAX;
        static thread_local uint32_t lastSeed = 0;
        uint32_t h = seed * 2654435761u; h ^= h >> 16;
        if (lastN != n || (lastSeed != seed && (h >> 26) == 0))
            SliceLog("[trainorder] seed=%u n=%zu named=%lld reordered=%d ids=%08x%s%s\n",
                     seed, n, (long long)outcome.named, outcome.changed,
                     TrainOrderIdHash(order.data(), int64_t(n), recs.data()),
                     outcome.duplicates ? " DUPLICATE-IDS" : "", type < 0 ? " NO-NAMES" : "");
        lastN = n; lastSeed = seed;
        return true;
    } catch (...) { return false; } // only our own allocations; no foreign calls
}


}

// The relay tail-jumps here, retaining the game's original return address and
// stack alignment. Foreign calls are deliberately outside the allocation catch.
extern "C" __attribute__((visibility("hidden")))
void SliceTrainOrder(int32_t* begin, int32_t* end, uint64_t* rng, uintptr_t self, uintptr_t world)
{
    ++steps;
    lastCount = (reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(begin)) / 4;
    const auto start = std::chrono::steady_clock::now();
    uintptr_t timeSystem = 0, timeArg = 0;
    bool ok = SliceReadT(self + 0x48, &timeSystem) && timeSystem &&
              SliceReadT(timeSystem + 0x18, &timeArg) && timeArg;
    uint32_t seed = 0;
    int type = -1;
    if (ok) {
        // Re-read raw uint32, not GCC's sign-extended/normalized seed: Windows
        // converts the accessor's int to uint32 before its minstd seed fixup.
        seed = reinterpret_cast<SeedGetter>(gameBase + 0xc0cf10)(reinterpret_cast<void*>(timeArg));
        const uintptr_t ti = gameBase + 0x5a02600;
        const uintptr_t node = reinterpret_cast<TypeFind>(gameBase + 0x9e3d50)(
            reinterpret_cast<void*>(world + 0x48), &ti);
        int stored = 0;
        if (node && SliceReadT(node + 0x10, &stored) && stored > 0 && stored <= 4097) type = stored - 1;
        lastSeedValue = seed;
        ok = Arrange(begin, end, self, world, type, seed);
    }
    if (!ok) {
        ++refusals;
        static bool warned = false;
        if (!warned) { SliceLog("[trainorder] REFUSED read/count/iota/allocation; original shuffle retained\n"); warned = true; }
        reinterpret_cast<Shuffle>(gameBase + 0x175a510)(begin, end, rng);
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    uint64_t previous = maxUs.load();
    while (uint64_t(us) > previous && !maxUs.compare_exchange_weak(previous, uint64_t(us))) {}
    static bool slow = false;
    if (us > 1000 && !slow) { SliceLog("[trainorder] SLOW %lld us\n", (long long)us); slow = true; }
}

// rdi/rsi/rdx already hold begin/end/RNG. r14 and the caller's frame hold
// self/world. No registers outside the SysV caller-saved set are modified.
extern "C" void SliceTrainOrderRelay();
asm(".text\n.hidden SliceTrainOrderRelay\n.type SliceTrainOrderRelay,@function\n"
    "SliceTrainOrderRelay:\n.cfi_startproc\n"
    "mov %r14,%rcx\nmov -0x198(%rbp),%r8\njmp SliceTrainOrder\n"
    ".cfi_endproc\n.size SliceTrainOrderRelay,.-SliceTrainOrderRelay\n");

bool SliceInstallTrainOrder(uintptr_t base, const char* rootDir, const char* dataDir)
{
    if (SliceFlagOff(rootDir, dataDir, "trainorder")) {
        SliceLog("[trainorder] OFF (trainorder=0); engine registration-order shuffle retained\n");
        return true;
    }
    for (const auto& check : kTrainOrderChecks) {
        uint8_t bytes[128];
        if (!SliceRead(base + check.rva, bytes, check.size) || memcmp(bytes, check.bytes, check.size)) {
            SliceLog("[trainorder] NOT installed: byte check failed at %lx\n", (unsigned long)check.rva);
            return false;
        }
    }
    // RTTI name is relocated in the loaded PIE: validate its target and text.
    uintptr_t name = 0;
    char text[sizeof("N3ecs9component4NameE")];
    if (!SliceReadT(base + 0x5a02608, &name) ||
        !SliceRead(name, text, sizeof(text)) || memcmp(text, "N3ecs9component4NameE", sizeof(text))) {
        SliceLog("[trainorder] NOT installed: Name RTTI check failed\n"); return false;
    }
    gameBase = base;
    if (!Tpf2mpRedirectCall(base + 0x175849d, base + 0x175a510, reinterpret_cast<void*>(&SliceTrainOrderRelay))) {
        SliceLog("[trainorder] NOT installed: shuffle call redirection failed\n"); return false;
    }
    installed = true;
    SliceLog("[trainorder] installed call=175849d resume=17584a2; name/id rank with seeded n/3 jitter\n");
    return true;
}

void SliceTrainOrderLogAlive()
{
    if (installed)
        SliceLog("[trainorder] alive: steps=%llu reorders=%llu refused=%llu lastSeed=%u lastN=%lld maxUs=%llu\n",
                 (unsigned long long)steps.load(), (unsigned long long)reorders.load(),
                 (unsigned long long)refusals.load(), lastSeedValue.load(),
                 (long long)lastCount.load(), (unsigned long long)maxUs.load());
}
