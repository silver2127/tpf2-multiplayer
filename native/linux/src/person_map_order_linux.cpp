#include "person_map_order_linux.h"
#include "hook.h"
#include "codewrite_linux.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace {
// Private records never own or change a game node. C allocation keeps all
// diagnostic/model callbacks nonthrowing, including the inline assembly paths.
struct OrderedNode { OrderedNode *next, *previous; uintptr_t gameNode; uint32_t key; };
struct Bucket { OrderedNode *first, *last; };
struct OrderedMap { OrderedNode *first, *last; Bucket* buckets; size_t count, bucketCount; };
uint64_t MapHash(uint32_t key)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (unsigned i = 0; i < 4; ++i) { hash ^= (key >> (8 * i)) & 255; hash *= UINT64_C(0x100000001b3); }
    return hash;
}
void ClearMap(OrderedMap& map)
{
    auto* node = map.first;
    while (node) { auto* next = node->next; std::free(node); node = next; }
    std::free(map.buckets); map = {};
}
OrderedNode* FindNode(OrderedMap& map, uint32_t key)
{
    if (!map.buckets) return nullptr;
    const auto& bucket = map.buckets[MapHash(key) & (map.bucketCount - 1)];
    if (!bucket.first) return nullptr;
    for (auto* p = bucket.first; ; p = p->next) {
        if (p->key == key) return p;
        if (p == bucket.last) break;
    }
    return nullptr;
}
void PlaceNode(OrderedMap& map, OrderedNode* node)
{
    auto& bucket = map.buckets[MapHash(node->key) & (map.bucketCount - 1)];
    if (!bucket.first) {
        node->previous = map.last; node->next = nullptr;
        if (map.last) map.last->next = node; else map.first = node;
        map.last = node; bucket.first = bucket.last = node;
    } else {
        node->next = bucket.first; node->previous = bucket.first->previous;
        if (node->previous) node->previous->next = node; else map.first = node;
        bucket.first->previous = node; bucket.first = node;
    }
}
bool AddNode(OrderedMap& map, uint32_t key, uintptr_t gameNode)
{
    if (auto* prior = FindNode(map, key)) return prior->gameNode == gameNode;
    if (!map.buckets) {
        map.buckets = static_cast<Bucket*>(std::calloc(8, sizeof(Bucket)));
        if (!map.buckets) return false;
        map.bucketCount = 8;
    }
    auto* node = static_cast<OrderedNode*>(std::calloc(1, sizeof(OrderedNode)));
    if (!node) return false;
    node->key = key; node->gameNode = gameNode; PlaceNode(map, node); ++map.count;
    if (map.count > map.bucketCount) {
        const size_t multiplier = map.bucketCount < 512 ? 8 : 2;
        if (map.bucketCount > SIZE_MAX / multiplier / sizeof(Bucket)) return false;
        const size_t count = map.bucketCount * multiplier;
        auto* buckets = static_cast<Bucket*>(std::calloc(count, sizeof(Bucket)));
        if (!buckets) return false;
        std::free(map.buckets); map.buckets = buckets; map.bucketCount = count;
        auto* current = map.first; map.first = map.last = nullptr;
        while (current) { auto* next = current->next; PlaceNode(map, current); current = next; }
    }
    return true;
}

constexpr size_t kInfoMapCount = 5, kNativeMapSize = 0x38;
struct MapOwner {
    MapOwner* next;
    uintptr_t data, owner;
    bool invalid, sealed;
    OrderedMap maps[kInfoMapCount];
};
// The original allocation/deleter, rather than an address timeout or thread
// identity, bounds each record. A mutex supports nested and transferred owners.
pthread_mutex_t g_mapMutex = PTHREAD_MUTEX_INITIALIZER;
MapOwner* g_mapOwners = nullptr;
std::atomic<const char*> g_mapStatus{"off (not initialized)"};
std::atomic<Tpf2mpPersonMapLog> g_mapLogger{nullptr};
std::atomic<unsigned> g_mapReportedErrors{0};
void MapError(const char* message)
{
    g_mapStatus.store(message, std::memory_order_relaxed);
    const auto log = g_mapLogger.load(std::memory_order_relaxed);
    if (!log) return;
    const char* failures[] = {
        "ERROR: person-map allocation reused without its verified deleter",
        "ERROR: person-map lifetime capture failed",
        "ERROR: person-map insertion capture failed",
        "ERROR: incomplete person-map capture; native traversal retained"
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        if (std::strcmp(message, failures[i])) continue;
        const unsigned bit = 1u << i;
        if (!(g_mapReportedErrors.fetch_or(bit, std::memory_order_relaxed) & bit))
            log("[person-map] %s\n", message);
        break;
    }
}
uintptr_t ReadPointer(uintptr_t address) { uintptr_t p; std::memcpy(&p, reinterpret_cast<void*>(address), 8); return p; }
uint32_t ReadKey(uintptr_t node) { uint32_t k; std::memcpy(&k, reinterpret_cast<void*>(node + 8), 4); return k; }
void DeleteOwner(MapOwner* owner)
{
    for (auto& map : owner->maps) ClearMap(map);
    std::free(owner);
}
void ForgetMaps(uintptr_t data)
{
    pthread_mutex_lock(&g_mapMutex);
    for (auto** at = &g_mapOwners; *at; at = &(*at)->next) {
        if ((*at)->data != data) continue;
        auto* old = *at; *at = old->next; DeleteOwner(old); break;
    }
    pthread_mutex_unlock(&g_mapMutex);
}
void BindMaps(uintptr_t data, uintptr_t owner)
{
    pthread_mutex_lock(&g_mapMutex);
    bool empty = data != 0;
    for (size_t i = 0; empty && i < kInfoMapCount; ++i)
        empty = !ReadPointer(data + i * kNativeMapSize + 0x10) && !ReadPointer(data + i * kNativeMapSize + 0x18);
    for (auto** at = &g_mapOwners; *at; at = &(*at)->next) {
        if ((*at)->data != data) continue;
        auto* old = *at; *at = old->next; DeleteOwner(old);
        MapError("ERROR: person-map allocation reused without its verified deleter"); break;
    }
    auto* record = empty ? static_cast<MapOwner*>(std::calloc(1, sizeof(MapOwner))) : nullptr;
    if (record) { record->data = data; record->owner = owner; record->next = g_mapOwners; g_mapOwners = record; }
    else MapError("ERROR: person-map lifetime capture failed");
    pthread_mutex_unlock(&g_mapMutex);
}
void CaptureMapInsertion(uintptr_t mapAddress, uint32_t key, uintptr_t node)
{
    pthread_mutex_lock(&g_mapMutex);
    for (auto* owner = g_mapOwners; owner; owner = owner->next) {
        if (mapAddress < owner->data || mapAddress - owner->data >= kInfoMapCount * kNativeMapSize ||
            (mapAddress - owner->data) % kNativeMapSize) continue;
        auto& map = owner->maps[(mapAddress - owner->data) / kNativeMapSize];
        if (owner->sealed || owner->invalid || !node || ReadKey(node) != key || !AddNode(map, key, node)) {
            owner->invalid = true; MapError("ERROR: person-map insertion capture failed");
        }
        break;
    }
    pthread_mutex_unlock(&g_mapMutex);
}
void SealOwner(MapOwner& owner)
{
    if (owner.sealed) return;
    owner.sealed = true;
    for (size_t i = 0; i < kInfoMapCount && !owner.invalid; ++i) {
        auto& map = owner.maps[i];
        const uintptr_t address = owner.data + i * kNativeMapSize;
        if (ReadPointer(address + 0x18) != map.count) { owner.invalid = true; break; }
        auto* p = map.first;
        while (p) { if (ReadKey(p->gameNode) != p->key) { owner.invalid = true; break; } p = p->next; }
        size_t visited = 0;
        for (uintptr_t native = ReadPointer(address + 0x10); native; native = ReadPointer(native)) {
            if (++visited > map.count) { owner.invalid = true; break; }
            auto* found = FindNode(map, ReadKey(native));
            if (!found || found->gameNode != native) { owner.invalid = true; break; }
        }
        if (visited != map.count) owner.invalid = true;
    }
    if (owner.invalid) MapError("ERROR: incomplete person-map capture; native traversal retained");
}
uintptr_t FirstMapNode(uintptr_t data, size_t index)
{
    uintptr_t result = ReadPointer(data + index * kNativeMapSize + 0x10);
    pthread_mutex_lock(&g_mapMutex);
    for (auto* owner = g_mapOwners; owner; owner = owner->next) if (owner->data == data) {
        SealOwner(*owner);
        if (!owner->invalid) result = owner->maps[index].first ? owner->maps[index].first->gameNode : 0;
        break;
    }
    pthread_mutex_unlock(&g_mapMutex); return result;
}
uintptr_t NextMapNode(uintptr_t node, size_t index)
{
    uintptr_t result = ReadPointer(node);
    pthread_mutex_lock(&g_mapMutex);
    for (auto* owner = g_mapOwners; owner; owner = owner->next) {
        if (!owner->sealed || owner->invalid) continue;
        auto* found = FindNode(owner->maps[index], ReadKey(node));
        if (found && found->gameNode == node) { result = found->next ? found->next->gameNode : 0; break; }
    }
    pthread_mutex_unlock(&g_mapMutex); return result;
}

constexpr char kMapBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
struct MapContext { uintptr_t rva; const unsigned char* bytes; size_t size; };
struct MapSite { uintptr_t rva; unsigned length, skip; unsigned char bytes[32]; };
#include "person_map_order_sites_linux.h"
constexpr size_t kMapHookCount = sizeof(kMapSites) / sizeof(kMapSites[0]);
static_assert(kMapHookCount == 28);
void* g_mapOriginal[kMapHookCount]{};
uintptr_t g_mapBase = 0;
unsigned g_mapActiveMask = 0;
struct MapRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(MapRegisters) == 136);
using NativeMapInsert = void* (*)(void*, const uint32_t*);
// No RAII object, catch block or LSDA surrounds the game call. A game allocation
// exception unwinds this ordinary CFI frame; the owner's verified deleter
// discards the entire record through the original constructor cleanup path.
__attribute__((noinline)) void* MapInsert(void* map, const uint32_t* key)
{
    const uint32_t value = *key;
    void* result = reinterpret_cast<NativeMapInsert>(g_mapOriginal[12])(map, key);
    CaptureMapInsertion(reinterpret_cast<uintptr_t>(map), value, reinterpret_cast<uintptr_t>(result) - 16);
    return result;
}
}

bool Tpf2mpWindowsPersonMapOrder(const uint32_t* ids, size_t count, uint32_t* output, size_t* outputCount)
{
    if (!outputCount || (count && (!ids || !output))) return false;
    OrderedMap map{}; bool ok = true;
    for (size_t i = 0; i < count; ++i) if (!AddNode(map, ids[i], uintptr_t(ids[i]) + 1)) { ok = false; break; }
    size_t n = 0;
    if (ok) for (auto* p = map.first; p; p = p->next) output[n++] = p->key;
    *outputCount = n; ClearMap(map); return ok;
}

extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpPersonMapDispatch(MapRegisters* r)
{
    const size_t index = r->resume;
    const auto& site = kMapSites[index];
    r->resume = g_mapBase + site.rva + site.length;
    if (index == 0) {
        BindMaps(r->rdx, r->r15);
        std::memcpy(reinterpret_cast<void*>(r->r15 + 0x120), &r->rdx, 8);
    } else if (index == 1) {
        ForgetMaps(r->rdi); r->resume = reinterpret_cast<uintptr_t>(g_mapOriginal[1]);
    } else if (index >= 2 && index <= 6) {
        uintptr_t data;
        if (index == 2) {
            const uint32_t eax = uint32_t(r->rax);
            std::memcpy(reinterpret_cast<void*>(r->rbp - 0x494), &eax, 4);
            data = r->r14; r->r13 = FirstMapNode(data, 0);
        } else {
            data = ReadPointer(r->rbp - 0x470); r->rax = data;
            const uintptr_t first = FirstMapNode(data, index - 2);
            if (index == 3) r->rbx = first;
            else if (index == 4) r->r12 = first;
            else if (index == 5) r->r14 = first;
            else { r->r13 = r->rbp - 0x39c; r->rbx = first; }
        }
    } else if (index >= 13 && index <= 17) {
        const size_t group = index - 13;
        uintptr_t data;
        if (group == 0) {
            data = r->r14;
            const uint32_t eax = uint32_t(r->rax);
            std::memcpy(reinterpret_cast<void*>(r->rbp - 0x198), &eax, 4);
        } else {
            data = ReadPointer(r->rbp - 0x1d0); r->rax = data;
        }
        const uintptr_t first = FirstMapNode(data, group);
        if (group == 3) r->r14 = first; else r->rbx = first;
    } else if (index >= 18) {
        const size_t group = (index - 18) / 2;
        uint64_t* value = group == 3 ? &r->r14 : &r->rbx;
        *value = NextMapNode(*value, group);
        r->resume = reinterpret_cast<uintptr_t>(g_mapOriginal[index]) + site.skip;
    } else {
        uint64_t* value = index == 7 ? &r->r13 : index == 9 ? &r->r12 : index == 10 ? &r->r14 : &r->rbx;
        *value = NextMapNode(*value, index - 7);
        // Replay each original TEST from its trampoline, preserving exact CPU
        // flags. The final site jumps to its original shared test block.
        r->resume = index == 11 ? g_mapBase + 0x2e6cf92
            : reinterpret_cast<uintptr_t>(g_mapOriginal[index]) + site.skip;
    }
}
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpPersonMapEntry()
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
        "stmxcsr 256(%rsp)\n\tmov %rbx, %rdi\n\tcall Tpf2mpPersonMapDispatch\n\tldmxcsr 256(%rsp)\n\t"
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
        // Restore the interrupted RSP without changing flags or a register.
        // Jump rather than synthesizing a return/shadow-stack entry.
        "lea 8(%rsp), %rsp\n\tjmp *-8(%rsp)\n\t");
}
namespace {
#define MAP_STUB(index) \
    __attribute__((naked, noinline)) void MapStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpPersonMapEntry\n\t"); \
    }
MAP_STUB(0) MAP_STUB(1) MAP_STUB(2) MAP_STUB(3) MAP_STUB(4) MAP_STUB(5)
MAP_STUB(6) MAP_STUB(7) MAP_STUB(8) MAP_STUB(9) MAP_STUB(10) MAP_STUB(11)
MAP_STUB(13) MAP_STUB(14) MAP_STUB(15) MAP_STUB(16) MAP_STUB(17) MAP_STUB(18) MAP_STUB(19) MAP_STUB(20) MAP_STUB(21) MAP_STUB(22) MAP_STUB(23) MAP_STUB(24) MAP_STUB(25) MAP_STUB(26) MAP_STUB(27)
#undef MAP_STUB
void* const kMapDetours[] = {
    reinterpret_cast<void*>(MapStub0), reinterpret_cast<void*>(MapStub1),
    reinterpret_cast<void*>(MapStub2), reinterpret_cast<void*>(MapStub3),
    reinterpret_cast<void*>(MapStub4), reinterpret_cast<void*>(MapStub5),
    reinterpret_cast<void*>(MapStub6), reinterpret_cast<void*>(MapStub7),
    reinterpret_cast<void*>(MapStub8), reinterpret_cast<void*>(MapStub9),
    reinterpret_cast<void*>(MapStub10), reinterpret_cast<void*>(MapStub11),
    reinterpret_cast<void*>(MapInsert),
    reinterpret_cast<void*>(MapStub13),
    reinterpret_cast<void*>(MapStub14),
    reinterpret_cast<void*>(MapStub15),
    reinterpret_cast<void*>(MapStub16),
    reinterpret_cast<void*>(MapStub17),
    reinterpret_cast<void*>(MapStub18),
    reinterpret_cast<void*>(MapStub19),
    reinterpret_cast<void*>(MapStub20),
    reinterpret_cast<void*>(MapStub21),
    reinterpret_cast<void*>(MapStub22),
    reinterpret_cast<void*>(MapStub23),
    reinterpret_cast<void*>(MapStub24),
    reinterpret_cast<void*>(MapStub25),
    reinterpret_cast<void*>(MapStub26),
    reinterpret_cast<void*>(MapStub27),
};
using MapHooker = bool (*)(uintptr_t, void*, int, void**);
using MapWriter = int (*)(uintptr_t, const uint8_t*, size_t, int*);
bool InstallPersonMaps(uintptr_t base, const char* buildId, MapHooker hook, MapWriter write)
{
    if (g_mapActiveMask) return false;
    if (!base || !buildId || std::strcmp(buildId, kMapBuildId)) { MapError("off (unverified image)"); return false; }
    for (const auto& c : kMapContexts) if (std::memcmp(reinterpret_cast<void*>(base + c.rva), c.bytes, c.size)) {
        MapError("off (unverified person-map construction/traversal/cleanup)"); return false;
    }
    g_mapBase = base;
    for (size_t i = 0; i < kMapHookCount; ++i) {
        const auto& site = kMapSites[i];
        if (hook(base + site.rva, kMapDetours[i], site.length, &g_mapOriginal[i])) {
            g_mapActiveMask |= 1u << i; continue;
        }
        g_mapActiveMask = 0;
        for (size_t j = 0; j <= i; ++j) {
            const auto& old = kMapSites[j]; int error = 0;
            write(base + old.rva, old.bytes, old.length, &error);
            if (std::memcmp(reinterpret_cast<void*>(base + old.rva), old.bytes, old.length)) g_mapActiveMask |= 1u << j;
        }
        MapError(g_mapActiveMask ? "ERROR: person-map hook rollback incomplete" : "off (person-map installation failed; original bytes restored)");
        return false;
    }
    MapError("enabled (Windows order in preparation and application of 5 private person maps)"); return true;
}
}
bool Tpf2mpInstallPersonMapOrder(uintptr_t imageBase, const char* buildId)
{
    return InstallPersonMaps(imageBase, buildId, InstallHook, Tpf2mpCodeWriteSelf);
}
const char* Tpf2mpPersonMapOrderStatus() { return g_mapStatus.load(std::memory_order_relaxed); }

void Tpf2mpPersonMapOrderSetLog(Tpf2mpPersonMapLog log) { g_mapLogger.store(log, std::memory_order_relaxed); }
