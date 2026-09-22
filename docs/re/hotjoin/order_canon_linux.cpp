#include "order_canon_linux.h"
#include "hook.h"
#include "codewrite_linux.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {
constexpr char kCanonBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";

// Each site: 7 displaced bytes, all position-independent, replayed verbatim by
// the trampoline; the entity vector's begin/end are rbp-relative there.
struct CanonSite {
    const char* name;
    uintptr_t rva;
    int steal;
    int32_t beginOff;   // vector<ecs::Entity> {begin, end, cap} at rbp+beginOff
    unsigned char bytes[16];
    int32_t derefOff;   // when nonzero: the vector is at [rbp+beginOff] + derefOff (inside a system)
    bool relinkMaps;    // not a vector: [rbp+beginOff] is a SimEntityUpdateHelper; relink its 9 maps
};
const CanonSite kCanonSites[] = {
    // GetTargetsByLandUse 0x1502600: every path joins at the output's
    // `mov qword [rbx],0`; the PersonCapacity copy is complete, unconsumed.
    { "candidates", 0x1502918, 7, -0x90,
      { 0x48,0xc7,0x03,0x00,0x00,0x00,0x00, 0x4c,0x8d,0x63,0x30, 0x48,0xc7,0x43,0x08,0x00 } },
    // SimEntityAtBuildingSystem::Update2 0x16f0ae0: the leave batch, just
    // before the signal's slot list is read (the signal call is at 0x16f0bd8).
    { "departures", 0x16f0bcb, 7, -0x50,
      { 0x48,0x8b,0x7a,0x08, 0x48,0x85,0xff, 0x74,0x5d, 0x48,0x8d,0x75,0xb0, 0xe8,0x03,0xb0 } },
    // PersonMoveSystem::Update2 0x16b5790: the walk-arrival vector (tuple +0x18
    // at rbp-0xa0) before NoteWalkPersonsArrived's signal (payload rbx+0x18).
    { "arrivals", 0x16b5dc6, 7, -0x88,
      { 0x48,0x8b,0x78,0x08, 0x48,0x85,0xff, 0x0f,0x84,0x06,0x02,0x00,0x00, 0x48,0x8d,0x9d } },
    // SimEntityIdleSystem::Update 0x1700540 (odd steps): the pending-idle list at
    // system+0x18, kept in insertion order, walked by PathFactory::Compute whose
    // chunk-seeded MT draws per item; the system is at [rbp-0x180].
    { "idle", 0x17005cc, 7, -0x180,
      { 0x48,0x8b,0x8d,0x80,0xfe,0xff,0xff, 0x48,0xc7,0x85,0x10,0xff,0xff,0xff,0x00,0x00 }, 0x18 },
    // SimEntityUpdateHelper apply 0x2e6e0c0 (every build / replace / demolish,
    // town growth included): after the generator is seeded (0x2e6e617), before
    // the person (0x2e6e75c) and cargo applies walk their maps with it and free
    // ids in walk order. Helper at [rbp-0xbd8], data block D = [helper+0x120].
    { "capacity-maps", 0x2e6e6c3, 7, -0xbd8,
      { 0x48,0x8b,0x85,0x28,0xf4,0xff,0xff, 0x48,0x8b,0x70,0x70, 0x4c,0x8b,0xa8,0x20,0x01 }, 0, true },
};
constexpr unsigned kCanonSiteCount = sizeof(kCanonSites) / sizeof(kCanonSites[0]);

void* g_canonOriginal[kCanonSiteCount]{};
bool g_canonReady = false;
std::atomic<const char*> g_canonStatus{"off (not initialized)"};
std::atomic<Tpf2mpOrderCanonLog> g_canonLog{nullptr};
struct CanonCounters { std::atomic<uint64_t> calls{0}, reordered{0}, refused{0}; };
CanonCounters g_canonCounters[kCanonSiteCount];

struct CanonRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(CanonRegisters) == 136);

// libstdc++ unordered_map<Entity, Info> inside the helper's data block: the
// first node at map+0x10, the count at map+0x18, node {next +0, int32 key +8}.
// Relinked in ascending key order; after this point the maps are only walked
// head to tail and cleared by walking `next` (RE 2026-09-22), so the stale
// bucket pointers are never read.
constexpr uint32_t kMapOffsets[9] = { 0x00, 0x38, 0x70, 0xa8, 0xe0, 0x118, 0x150, 0x188, 0x1c0 };
constexpr size_t kMapMaxNodes = 1u << 20;
bool RelinkMap(uintptr_t map) noexcept
{
    size_t count;
    std::memcpy(&count, reinterpret_cast<const void*>(map + 0x18), sizeof(count));
    if (count < 2) return true;
    if (count > kMapMaxNodes) return false;
    auto* nodes = static_cast<uintptr_t*>(std::malloc(count * sizeof(uintptr_t)));
    if (!nodes) return false;
    size_t n = 0;
    uintptr_t node;
    std::memcpy(&node, reinterpret_cast<const void*>(map + 0x10), sizeof(node));
    while (node && n <= count) {
        if (n == count) { n = count + 1; break; }
        nodes[n++] = node;
        std::memcpy(&node, reinterpret_cast<const void*>(node), sizeof(node));
    }
    if (n != count) { std::free(nodes); return false; }
    auto key = [](uintptr_t p) { int32_t k; std::memcpy(&k, reinterpret_cast<const void*>(p + 8), 4); return k; };
    std::sort(nodes, nodes + n, [&](uintptr_t a, uintptr_t b) { return key(a) < key(b); });
    std::memcpy(reinterpret_cast<void*>(map + 0x10), &nodes[0], sizeof(uintptr_t));
    for (size_t i = 0; i < n; ++i) {
        const uintptr_t next = i + 1 < n ? nodes[i + 1] : 0;
        std::memcpy(reinterpret_cast<void*>(nodes[i]), &next, sizeof(next));
    }
    std::free(nodes);
    return true;
}

void CanonRelink(unsigned site, uintptr_t rbp) noexcept
{
    auto& c = g_canonCounters[site];
    const uint64_t n = c.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    uintptr_t helper = 0, data = 0;
    std::memcpy(&helper, reinterpret_cast<const void*>(rbp + kCanonSites[site].beginOff), sizeof(helper));
    if (helper) std::memcpy(&data, reinterpret_cast<const void*>(helper + 0x120), sizeof(data));
    if (!data) { c.refused.fetch_add(1, std::memory_order_relaxed); return; }
    unsigned bad = 0;
    for (uint32_t off : kMapOffsets) if (!RelinkMap(data + off)) ++bad;
    if (bad) {
        if (!c.refused.fetch_add(1, std::memory_order_relaxed))
            if (auto log = g_canonLog.load(std::memory_order_relaxed))
                log("[order-canon] ERROR: capacity-maps: %u of 9 maps not walkable (count/list mismatch); left as the engine built them\n", bad);
    } else {
        c.reordered.fetch_add(1, std::memory_order_relaxed);
    }
    if (n == 1 || !(n & 0xfff))
        if (auto log = g_canonLog.load(std::memory_order_relaxed))
            log("[order-canon] alive: capacity-maps applies=%llu relinked=%llu refused=%llu\n", (unsigned long long)n,
                (unsigned long long)c.reordered.load(std::memory_order_relaxed),
                (unsigned long long)c.refused.load(std::memory_order_relaxed));
}

void CanonSort(unsigned site, uintptr_t rbp) noexcept
{
    if (kCanonSites[site].relinkMaps) { CanonRelink(site, rbp); return; }
    auto& c = g_canonCounters[site];
    const uint64_t n = c.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    uintptr_t at = rbp + kCanonSites[site].beginOff;
    if (kCanonSites[site].derefOff) {
        uintptr_t sys;
        std::memcpy(&sys, reinterpret_cast<const void*>(at), sizeof(sys));
        if (!sys) { c.refused.fetch_add(1, std::memory_order_relaxed); return; }
        at = sys + kCanonSites[site].derefOff;
    }
    uintptr_t v[3];
    std::memcpy(v, reinterpret_cast<const void*>(at), sizeof(v));
    const uintptr_t begin = v[0], end = v[1], cap = v[2];
    if (begin > end || end > cap || ((end - begin) & 3) || (begin & 3) || end - begin > (uintptr_t(1) << 28)
        || (begin == 0) != (end == 0)) {
        if (!c.refused.fetch_add(1, std::memory_order_relaxed)) {
            if (auto log = g_canonLog.load(std::memory_order_relaxed))
                log("[order-canon] ERROR: %s: not an entity vector at rbp%+d (%p %p %p); left as is\n",
                    kCanonSites[site].name, kCanonSites[site].beginOff, (void*)begin, (void*)end, (void*)cap);
        }
        return;
    }
    int32_t* b = reinterpret_cast<int32_t*>(begin);
    int32_t* e = reinterpret_cast<int32_t*>(end);
    if (!std::is_sorted(b, e)) {
        std::sort(b, e);
        c.reordered.fetch_add(1, std::memory_order_relaxed);
    }
    // alive line: first call, then every 2^16 calls per site
    if (n == 1 || !(n & 0xffff)) {
        if (auto log = g_canonLog.load(std::memory_order_relaxed))
            log("[order-canon] alive: %s calls=%llu reordered=%llu refused=%llu last=%lld\n", kCanonSites[site].name,
                (unsigned long long)n, (unsigned long long)c.reordered.load(std::memory_order_relaxed),
                (unsigned long long)c.refused.load(std::memory_order_relaxed), (long long)(e - b));
    }
}
}

extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpOrderCanonDispatch(CanonRegisters* r) noexcept
{
    const size_t index = r->resume;
    r->resume = reinterpret_cast<uintptr_t>(g_canonOriginal[index]);
    if (g_canonReady && index < kCanonSiteCount) CanonSort(unsigned(index), r->rbp);
}

// Same register-preserving entry as target_order_linux.cpp: all GP registers,
// flags, XMM0-15 and MXCSR survive; the trampoline replays the displaced bytes.
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpOrderCanonEntry()
{
    __asm__(
        "pushfq\n\t"
        "push %rax\n\tpush %rcx\n\tpush %rdx\n\tpush %rsi\n\tpush %rdi\n\t"
        "push %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n\tpush %rbx\n\t"
        "push %rbp\n\tpush %r12\n\tpush %r13\n\tpush %r14\n\tpush %r15\n\t"
        "mov %rsp, %rbx\n\tand $-16, %rsp\n\tsub $272, %rsp\n\t"
        "movdqu %xmm0, 0(%rsp)\n\tmovdqu %xmm1, 16(%rsp)\n\t"
        "movdqu %xmm2, 32(%rsp)\n\tmovdqu %xmm3, 48(%rsp)\n\t"
        "movdqu %xmm4, 64(%rsp)\n\tmovdqu %xmm5, 80(%rsp)\n\t"
        "movdqu %xmm6, 96(%rsp)\n\tmovdqu %xmm7, 112(%rsp)\n\t"
        "movdqu %xmm8, 128(%rsp)\n\tmovdqu %xmm9, 144(%rsp)\n\t"
        "movdqu %xmm10, 160(%rsp)\n\tmovdqu %xmm11, 176(%rsp)\n\t"
        "movdqu %xmm12, 192(%rsp)\n\tmovdqu %xmm13, 208(%rsp)\n\t"
        "movdqu %xmm14, 224(%rsp)\n\tmovdqu %xmm15, 240(%rsp)\n\t"
        "stmxcsr 256(%rsp)\n\tmov %rbx, %rdi\n\tcall Tpf2mpOrderCanonDispatch\n\tldmxcsr 256(%rsp)\n\t"
        "movdqu 0(%rsp), %xmm0\n\tmovdqu 16(%rsp), %xmm1\n\t"
        "movdqu 32(%rsp), %xmm2\n\tmovdqu 48(%rsp), %xmm3\n\t"
        "movdqu 64(%rsp), %xmm4\n\tmovdqu 80(%rsp), %xmm5\n\t"
        "movdqu 96(%rsp), %xmm6\n\tmovdqu 112(%rsp), %xmm7\n\t"
        "movdqu 128(%rsp), %xmm8\n\tmovdqu 144(%rsp), %xmm9\n\t"
        "movdqu 160(%rsp), %xmm10\n\tmovdqu 176(%rsp), %xmm11\n\t"
        "movdqu 192(%rsp), %xmm12\n\tmovdqu 208(%rsp), %xmm13\n\t"
        "movdqu 224(%rsp), %xmm14\n\tmovdqu 240(%rsp), %xmm15\n\t"
        "mov %rbx, %rsp\n\t"
        "pop %r15\n\tpop %r14\n\tpop %r13\n\tpop %r12\n\tpop %rbp\n\t"
        "pop %rbx\n\tpop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\t"
        "pop %rdi\n\tpop %rsi\n\tpop %rdx\n\tpop %rcx\n\tpop %rax\n\tpopfq\n\t"
        "lea 8(%rsp), %rsp\n\tjmp *-8(%rsp)\n\t");
}

namespace {
#define CANON_STUB(index) \
    __attribute__((naked, noinline)) void CanonStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpOrderCanonEntry\n\t"); \
    }
CANON_STUB(0) CANON_STUB(1) CANON_STUB(2) CANON_STUB(3) CANON_STUB(4)
#undef CANON_STUB
void* const kCanonDetours[] = {
    reinterpret_cast<void*>(CanonStub0), reinterpret_cast<void*>(CanonStub1), reinterpret_cast<void*>(CanonStub2),
    reinterpret_cast<void*>(CanonStub3), reinterpret_cast<void*>(CanonStub4)
};
static_assert(sizeof(kCanonDetours) / sizeof(kCanonDetours[0]) == kCanonSiteCount);
}

bool Tpf2mpInstallOrderCanon(uintptr_t base, const char* buildId)
{
    if (g_canonReady) return false;
    const char* env = std::getenv("TPF2MP_ORDER_CANON");
    if (env && !std::strcmp(env, "0")) { g_canonStatus.store("off (TPF2MP_ORDER_CANON=0)"); return false; }
    if (!base || !buildId || std::strcmp(buildId, kCanonBuildId)) { g_canonStatus.store("off (unverified image)"); return false; }
    for (const auto& site : kCanonSites)
        if (std::memcmp(reinterpret_cast<const void*>(base + site.rva), site.bytes, sizeof(site.bytes))) {
            g_canonStatus.store("off (unverified site bytes)"); return false;
        }
    unsigned installed = 0;
    for (unsigned i = 0; i < kCanonSiteCount; ++i) {
        if (InstallHook(base + kCanonSites[i].rva, kCanonDetours[i], kCanonSites[i].steal, &g_canonOriginal[i])) {
            ++installed; continue;
        }
        // roll back: put every earlier site's original bytes back
        for (unsigned j = 0; j < i; ++j) {
            int error = 0;
            Tpf2mpCodeWriteSelf(base + kCanonSites[j].rva, kCanonSites[j].bytes, kCanonSites[j].steal, &error);
        }
        g_canonStatus.store("off (hook installation failed; original bytes restored)");
        return false;
    }
    g_canonReady = true;
    g_canonStatus.store("enabled (candidates, departures, arrivals, idle sorted by entity id; capacity maps relinked)");
    return installed == kCanonSiteCount;
}
const char* Tpf2mpOrderCanonStatus() { return g_canonStatus.load(std::memory_order_relaxed); }
void Tpf2mpOrderCanonSetLog(Tpf2mpOrderCanonLog log) { g_canonLog.store(log, std::memory_order_relaxed); }
void Tpf2mpOrderCanonCounters(unsigned site, uint64_t* calls, uint64_t* reordered, uint64_t* refused)
{
    if (site >= kCanonSiteCount) { *calls = *reordered = *refused = 0; return; }
    *calls = g_canonCounters[site].calls.load(std::memory_order_relaxed);
    *reordered = g_canonCounters[site].reordered.load(std::memory_order_relaxed);
    *refused = g_canonCounters[site].refused.load(std::memory_order_relaxed);
}
