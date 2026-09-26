#include "target_order_linux.h"
#include "windows_entity_set_order_linux.h"
#include "hook.h"
#include "codewrite_linux.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace {
struct TargetRecord {
    TargetRecord* next;
    TargetRecord* previous;
    TargetRecord* hashNext;
    uint32_t target;
    uintptr_t nativeSet;
    bool invalid;
    Tpf2mpWindowsEntitySet order;
};
// Index private records by target; never change the native map or MSVC order.
// Fixed buckets need no allocation on lookup and survive churn until owner cleanup.
constexpr size_t kTargetBuckets = 1024;
size_t TargetBucket(uint32_t target) noexcept { return (target * UINT32_C(2654435761)) & (kTargetBuckets - 1); }
struct TargetOwner { TargetOwner* next; uintptr_t map; bool invalid; TargetRecord* records; TargetRecord* buckets[kTargetBuckets]; };
pthread_mutex_t g_targetMutex = PTHREAD_MUTEX_INITIALIZER;
TargetOwner* g_targetOwners = nullptr;
std::atomic<const char*> g_targetStatus{"off (not initialized)"};
std::atomic<Tpf2mpTargetOrderLog> g_targetLogger{nullptr};
std::atomic<unsigned> g_targetReportedErrors{0};
void TargetError(const char* message) noexcept
{
    g_targetStatus.store(message, std::memory_order_relaxed);
    const auto log = g_targetLogger.load(std::memory_order_relaxed);
    if (!log) return;
    const char* failures[] = {
        "ERROR: target-map owner reused without verified cleanup",
        "ERROR: target-map initialization capture failed",
        "ERROR: insertion into an untracked target-map owner",
        "ERROR: target-set history allocation failed",
        "ERROR: incomplete target-set insertion history",
        "ERROR: erase from an untracked target-map owner",
        "ERROR: missing target-set erase history",
        "ERROR: inconsistent target-set erase history",
        "ERROR: incomplete target-set capture; native traversal retained",
        "ERROR: untracked nonempty target-set traversal"
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        if (std::strcmp(message, failures[i])) continue;
        const unsigned bit = 1u << i;
        if (!(g_targetReportedErrors.fetch_or(bit, std::memory_order_relaxed) & bit))
            log("[target-order] %s\n", message);
        break;
    }
}
uintptr_t TargetRead(uintptr_t address) noexcept { uintptr_t value; std::memcpy(&value, reinterpret_cast<void*>(address), 8); return value; }
uint32_t TargetKey(uintptr_t node) noexcept { uint32_t value; std::memcpy(&value, reinterpret_cast<void*>(node + 8), 4); return value; }
void DeleteTargetRecord(TargetRecord* record) noexcept { Tpf2mpWindowsEntitySetClear(record->order); std::free(record); }
void DeleteTargetOwner(TargetOwner* owner) noexcept
{
    for (auto* record = owner->records; record;) { auto* next = record->next; DeleteTargetRecord(record); record = next; }
    std::free(owner);
}
void ForgetTargetOwner(uintptr_t map) noexcept
{
    pthread_mutex_lock(&g_targetMutex);
    for (auto** at = &g_targetOwners; *at; at = &(*at)->next) if ((*at)->map == map) {
        auto* old = *at; *at = old->next; DeleteTargetOwner(old); break;
    }
    pthread_mutex_unlock(&g_targetMutex);
}
void BindTargetOwner(uintptr_t map) noexcept
{
    pthread_mutex_lock(&g_targetMutex);
    for (auto** at = &g_targetOwners; *at; at = &(*at)->next) if ((*at)->map == map) {
        auto* old = *at; *at = old->next; DeleteTargetOwner(old);
        TargetError("ERROR: target-map owner reused without verified cleanup"); break;
    }
    const bool empty = !TargetRead(map + 0x10) && !TargetRead(map + 0x18);
    auto* owner = empty ? static_cast<TargetOwner*>(std::calloc(1, sizeof(TargetOwner))) : nullptr;
    if (owner) { owner->map = map; owner->next = g_targetOwners; g_targetOwners = owner; }
    else TargetError("ERROR: target-map initialization capture failed");
    pthread_mutex_unlock(&g_targetMutex);
}
using TargetLookup = void* (*)(void*, const uint32_t*);
TargetLookup g_targetLookup = nullptr;
void ObserveTargetInsert(uintptr_t map, uint32_t target, uint32_t id) noexcept
{
    const uintptr_t outerNode = reinterpret_cast<uintptr_t>(g_targetLookup(reinterpret_cast<void*>(map), &target));
    const uintptr_t set = outerNode ? outerNode + 0x10 : 0;
    uintptr_t inserted = 0;
    size_t visited = 0;
    const size_t count = set ? TargetRead(set + 0x18) : 0;
    for (uintptr_t node = set ? TargetRead(set + 0x10) : 0; node && visited++ < count; node = TargetRead(node))
        if (TargetKey(node) == id) { inserted = node; break; }
    pthread_mutex_lock(&g_targetMutex);
    TargetOwner* owner = g_targetOwners;
    while (owner && owner->map != map) owner = owner->next;
    if (!owner) TargetError("ERROR: insertion into an untracked target-map owner");
    else if (!owner->invalid) {
        auto& head = owner->buckets[TargetBucket(target)];
        TargetRecord* record = head;
        while (record && record->target != target) record = record->hashNext;
        if (!record) {
            record = static_cast<TargetRecord*>(std::calloc(1, sizeof(TargetRecord)));
            if (record) { record->target = target; record->nativeSet = set; record->hashNext = head; head = record;
                record->next = owner->records; if(record->next)record->next->previous=record; owner->records=record; }
            else { owner->invalid = true; TargetError("ERROR: target-set history allocation failed"); }
        }
        if (record && !record->invalid && (!inserted || record->nativeSet != set ||
            count != record->order.count + 1 ||
            !Tpf2mpWindowsEntitySetInsert(record->order, id, inserted))) {
            record->invalid = true; TargetError("ERROR: incomplete target-set insertion history");
        }
    }
    pthread_mutex_unlock(&g_targetMutex);
}
void ObserveTargetErase(uintptr_t map, uint32_t target, uint32_t id) noexcept
{
    // The original may already have freed the last resident node, its set and
    // outer-map entry. Only private metadata is inspected after that call.
    pthread_mutex_lock(&g_targetMutex);
    TargetOwner* owner = g_targetOwners;
    while (owner && owner->map != map) owner = owner->next;
    if (!owner) TargetError("ERROR: erase from an untracked target-map owner");
    else if (!owner->invalid) {
        auto** at = &owner->buckets[TargetBucket(target)];
        while (*at && (*at)->target != target) at = &(*at)->hashNext;
        if (!*at) { owner->invalid = true; TargetError("ERROR: missing target-set erase history"); }
        else {
            auto* record = *at;
            if (!record->invalid && !Tpf2mpWindowsEntitySetErase(record->order, id)) {
                record->invalid = true; TargetError("ERROR: inconsistent target-set erase history");
            }
            if (!record->invalid && !record->order.count) {
                *at = record->hashNext;
                if(record->previous)record->previous->next=record->next;else owner->records=record->next;
                if(record->next)record->next->previous=record->previous;
                DeleteTargetRecord(record);
            }
        }
    }
    pthread_mutex_unlock(&g_targetMutex);
}
bool ValidateTargetRecord(TargetRecord& record) noexcept
{
    if (record.invalid) return false;
    const size_t count = TargetRead(record.nativeSet + 0x18);
    if (count != record.order.count) record.invalid = true;
    size_t visited = 0;
    for (uintptr_t node = TargetRead(record.nativeSet + 0x10); !record.invalid && node; node = TargetRead(node)) {
        auto* found = Tpf2mpWindowsEntitySetFind(record.order, TargetKey(node));
        if (++visited > count || !found || found->nativeNode != node) record.invalid = true;
    }
    if (visited != count) record.invalid = true;
    if (record.invalid) TargetError("ERROR: incomplete target-set capture; native traversal retained");
    return !record.invalid;
}
uintptr_t FirstTargetNode(uintptr_t set) noexcept
{
    uintptr_t result = TargetRead(set + 0x10);
    if (!result) return 0;
    bool found = false;
    pthread_mutex_lock(&g_targetMutex);
    for (auto* owner = g_targetOwners; owner; owner = owner->next)
        for (auto* record = owner->records; record; record = record->next)
            if (record->nativeSet == set) {
                found = true;
                if (!owner->invalid && ValidateTargetRecord(*record))
                    result = record->order.first ? record->order.first->nativeNode : 0;
            }
    if (!found) TargetError("ERROR: untracked nonempty target-set traversal");
    pthread_mutex_unlock(&g_targetMutex); return result;
}
uintptr_t NextTargetNode(uintptr_t node) noexcept
{
    uintptr_t result = TargetRead(node);
    const uint32_t id = TargetKey(node);
    pthread_mutex_lock(&g_targetMutex);
    for (auto* owner = g_targetOwners; owner; owner = owner->next) if (!owner->invalid)
        for (auto* record = owner->records; record; record = record->next) if (!record->invalid) {
            auto* found = Tpf2mpWindowsEntitySetFind(record->order, id);
            if (found && found->nativeNode == node) result = found->next ? found->next->nativeNode : 0;
        }
    pthread_mutex_unlock(&g_targetMutex); return result;
}

constexpr char kTargetBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
struct TargetContext { uintptr_t rva; const unsigned char* bytes; size_t size; };
struct TargetSite { unsigned context; uintptr_t rva; int size; };
#include "target_order_sites_linux.h"
constexpr size_t kTargetSiteCount = sizeof(kTargetSites) / sizeof(kTargetSites[0]);
static_assert(kTargetSiteCount == 6);
void* g_targetOriginal[kTargetSiteCount]{};
uintptr_t g_targetBase = 0;
unsigned g_targetActiveMask = 0;
bool g_targetReady = false;
using TargetWrite = void (*)(const uint32_t*, const int32_t*, void*);
using TargetClear = void (*)(void*);
// No C++ cleanup object or held mutex surrounds any original engine call.
// If it throws, engine construction/destruction follows its original path;
// the exact outer-map cleanup removes our bookkeeping, including failed ctors.
void TargetInsert(const uint32_t* person, const int32_t* target, void* map)
{
    const uint32_t id = *person; const int32_t key = *target;
    reinterpret_cast<TargetWrite>(g_targetOriginal[2])(person, target, map);
    if (g_targetReady && key >= 0) ObserveTargetInsert(reinterpret_cast<uintptr_t>(map), uint32_t(key), id);
}
void TargetErase(const uint32_t* person, const int32_t* target, void* map)
{
    const uint32_t id = *person; const int32_t key = *target;
    reinterpret_cast<TargetWrite>(g_targetOriginal[3])(person, target, map);
    if (g_targetReady && key >= 0) ObserveTargetErase(reinterpret_cast<uintptr_t>(map), uint32_t(key), id);
}
void TargetClearOwner(void* map)
{
    if (g_targetReady) ForgetTargetOwner(reinterpret_cast<uintptr_t>(map));
    reinterpret_cast<TargetClear>(g_targetOriginal[1])(map);
}
struct TargetRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(TargetRegisters) == 136);
}
extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpTargetOrderDispatch(TargetRegisters* r) noexcept
{
    const size_t index = r->resume;
    r->resume = reinterpret_cast<uintptr_t>(g_targetOriginal[index]);
    if (!g_targetReady) return;
    if (index == 0) {
        BindTargetOwner(r->rdi + 0x1d8);
        std::memcpy(reinterpret_cast<void*>(r->rbp - 0x60), &r->rdi, 8);
        r->rdi = r->r12;
    } else if (index == 4) {
        r->rbx = FirstTargetNode(r->rax); r->rax = r->rbp - 0xb8;
    } else if (index == 5) {
        r->rbx = NextTargetNode(r->rbx);
        r->resume += 3; // original TEST RBX,RBX preserves exact branch flags
        return;
    }
    r->resume = g_targetBase + kTargetSites[index].rva + kTargetSites[index].size;
}
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpTargetOrderEntry()
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
        "stmxcsr 256(%rsp)\n\tmov %rbx, %rdi\n\tcall Tpf2mpTargetOrderDispatch\n\tldmxcsr 256(%rsp)\n\t"
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
#define TARGET_STUB(index) \
    __attribute__((naked, noinline)) void TargetStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpTargetOrderEntry\n\t"); \
    }
TARGET_STUB(0) TARGET_STUB(4) TARGET_STUB(5)
#undef TARGET_STUB
void* const kTargetDetours[] = {
    reinterpret_cast<void*>(TargetStub0), reinterpret_cast<void*>(TargetClearOwner),
    reinterpret_cast<void*>(TargetInsert), reinterpret_cast<void*>(TargetErase),
    reinterpret_cast<void*>(TargetStub4), reinterpret_cast<void*>(TargetStub5)
};
using TargetHooker = bool (*)(uintptr_t, void*, int, void**);
using TargetWriter = int (*)(uintptr_t, const uint8_t*, size_t, int*);
const unsigned char* TargetOriginalBytes(const TargetSite& site)
{
    const auto& context = kTargetContexts[site.context]; return context.bytes + site.rva - context.rva;
}
bool InstallTargetOrder(uintptr_t base, const char* buildId, TargetHooker hook, TargetWriter write)
{
    if (g_targetActiveMask) return false;
    if (!base || !buildId || std::strcmp(buildId, kTargetBuildId)) { TargetError("off (unverified image)"); return false; }
    for (const auto& context : kTargetContexts)
        if (std::memcmp(reinterpret_cast<void*>(base + context.rva), context.bytes, context.size)) {
            TargetError("off (unverified target-set lifetime/writer/reader)"); return false;
        }
    g_targetBase = base; g_targetLookup = reinterpret_cast<TargetLookup>(base + 0xf040c0);
    for (size_t i = 0; i < kTargetSiteCount; ++i) {
        const auto& site = kTargetSites[i];
        if (hook(base + site.rva, kTargetDetours[i], site.size, &g_targetOriginal[i])) {
            g_targetActiveMask |= 1u << i; continue;
        }
        g_targetReady = false; g_targetActiveMask = 0;
        for (size_t j = 0; j <= i; ++j) {
            const auto& old = kTargetSites[j]; int error = 0;
            write(base + old.rva, TargetOriginalBytes(old), old.size, &error);
            if (std::memcmp(reinterpret_cast<void*>(base + old.rva), TargetOriginalBytes(old), old.size))
                g_targetActiveMask |= 1u << j;
        }
        TargetError(g_targetActiveMask ? "ERROR: target-set hook rollback incomplete"
                                      : "off (target-set installation failed; original bytes restored)");
        return false;
    }
    g_targetReady = true;
    TargetError("enabled (Windows target-set history and capacity traversal)"); return true;
}
}
bool Tpf2mpInstallTargetOrder(uintptr_t imageBase, const char* buildId)
{
    return InstallTargetOrder(imageBase, buildId, InstallHook, Tpf2mpCodeWriteSelf);
}
const char* Tpf2mpTargetOrderStatus() { return g_targetStatus.load(std::memory_order_relaxed); }

void Tpf2mpTargetOrderSetLog(Tpf2mpTargetOrderLog log) { g_targetLogger.store(log, std::memory_order_relaxed); }
