#include "network_person_order_linux.h"
#include "windows_entity_set_order_linux.h"
#include "hook.h"
#include "codewrite_linux.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace {
struct NetworkRecord {
    NetworkRecord* next;
    uintptr_t nativeSet;
    Tpf2mpWindowsEntitySet order;
};
struct NetworkIndex { uint32_t id; uintptr_t node; };
pthread_mutex_t g_networkMutex = PTHREAD_MUTEX_INITIALIZER;
NetworkRecord* g_networkRecords = nullptr;
std::atomic<const char*> g_networkStatus{"off (not initialized)"};
std::atomic<Tpf2mpNetworkPersonLog> g_networkLog{nullptr};
std::atomic<bool> g_networkReported{false};

uintptr_t NetworkPointer(uintptr_t address)
{
    uintptr_t result; std::memcpy(&result, reinterpret_cast<void*>(address), sizeof(result)); return result;
}
uint32_t NetworkKey(uintptr_t address)
{
    uint32_t result; std::memcpy(&result, reinterpret_cast<void*>(address), sizeof(result)); return result;
}
void NetworkFailure()
{
    g_networkStatus.store("ERROR: incomplete affected-network collection; native traversal retained", std::memory_order_relaxed);
    const auto log = g_networkLog.load(std::memory_order_relaxed);
    if (log && !g_networkReported.exchange(true, std::memory_order_relaxed))
        log("[network-person] ERROR: incomplete affected-network collection; native traversal retained\n");
}
void DeleteNetworkRecord(NetworkRecord* record)
{
    Tpf2mpWindowsEntitySetClear(record->order); std::free(record);
}
void ForgetNetworkSet(uintptr_t set)
{
    pthread_mutex_lock(&g_networkMutex);
    for (auto** at = &g_networkRecords; *at; at = &(*at)->next) {
        if ((*at)->nativeSet != set) continue;
        auto* old = *at; *at = old->next; DeleteNetworkRecord(old); break;
    }
    pthread_mutex_unlock(&g_networkMutex);
}
int CompareNetworkIndex(const void* left, const void* right)
{
    const uint32_t a = static_cast<const NetworkIndex*>(left)->id;
    const uint32_t b = static_cast<const NetworkIndex*>(right)->id;
    return a < b ? -1 : a != b;
}
// Reconstruct the fresh Windows set from the completed source batches. No
// ordering inference from native buckets or recovered allocation history is
// needed. The native set supplies only the exact ID -> original node mapping.
NetworkRecord* BuildNetworkRecord(uintptr_t set, uintptr_t batches)
{
    const size_t count = NetworkPointer(set + 0x18);
    constexpr size_t kMaxItems = size_t(1) << 24;
    if (count > kMaxItems) return nullptr;
    auto* record = static_cast<NetworkRecord*>(std::calloc(1, sizeof(NetworkRecord)));
    auto* index = count ? static_cast<NetworkIndex*>(std::malloc(count * sizeof(NetworkIndex))) : nullptr;
    if (!record || (count && !index)) { std::free(record); std::free(index); return nullptr; }
    record->nativeSet = set;
    size_t visited = 0;
    uintptr_t node = NetworkPointer(set + 0x10);
    while (node && visited < count) {
        index[visited++] = {NetworkKey(node + 8), node}; node = NetworkPointer(node);
    }
    bool valid = !node && visited == count;
    if (valid && count) std::qsort(index, count, sizeof(NetworkIndex), CompareNetworkIndex);
    for (size_t i = 1; valid && i < count; ++i) valid = index[i - 1].id != index[i].id;
    const uintptr_t begin = NetworkPointer(batches), end = NetworkPointer(batches + 8);
    const uintptr_t capacity = NetworkPointer(batches + 16);
    valid = valid && end >= begin && capacity >= end && (end - begin) % 24 == 0
        && (end - begin) / 24 <= kMaxItems;
    size_t total = 0;
    for (uintptr_t batch = begin; valid && batch != end; batch += 24) {
        const uintptr_t first = NetworkPointer(batch), last = NetworkPointer(batch + 8);
        const uintptr_t limit = NetworkPointer(batch + 16);
        valid = last >= first && limit >= last && (last - first) % 4 == 0
            && (last - first) / 4 <= kMaxItems - total;
        if (!valid) break;
        total += (last - first) / 4;
        for (uintptr_t p = first; valid && p != last; p += 4) {
            const NetworkIndex key{NetworkKey(p), 0};
            const auto* found = count ? static_cast<const NetworkIndex*>(
                std::bsearch(&key, index, count, sizeof(NetworkIndex), CompareNetworkIndex)) : nullptr;
            valid = found && Tpf2mpWindowsEntitySetInsert(record->order, key.id, found->node);
        }
    }
    valid = valid && record->order.count == count;
    std::free(index);
    if (!valid) { DeleteNetworkRecord(record); return nullptr; }
    return record;
}
uintptr_t FirstNetworkNode(uintptr_t set, uintptr_t batches)
{
    const uintptr_t fallback = NetworkPointer(set + 0x10);
    ForgetNetworkSet(set);
    auto* record = BuildNetworkRecord(set, batches);
    if (!record) { NetworkFailure(); return fallback; }
    const uintptr_t first = record->order.first ? record->order.first->nativeNode : 0;
    pthread_mutex_lock(&g_networkMutex);
    record->next = g_networkRecords; g_networkRecords = record;
    pthread_mutex_unlock(&g_networkMutex);
    return first;
}
uintptr_t NextNetworkNode(uintptr_t set, uintptr_t node)
{
    uintptr_t result = NetworkPointer(node);
    pthread_mutex_lock(&g_networkMutex);
    for (auto* record = g_networkRecords; record; record = record->next) {
        if (record->nativeSet != set) continue;
        auto* entry = Tpf2mpWindowsEntitySetFind(record->order, NetworkKey(node + 8));
        if (entry && entry->nativeNode == node)
            result = entry->next ? entry->next->nativeNode : 0;
        break;
    }
    pthread_mutex_unlock(&g_networkMutex);
    return result;
}

constexpr char kNetworkBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
struct NetworkContext { uintptr_t rva; const unsigned char* bytes; size_t size; };
struct NetworkSite { uintptr_t rva; unsigned length; unsigned char bytes[16]; };
#include "network_person_order_sites_linux.h"
constexpr size_t kNetworkHookCount = sizeof(kNetworkSites) / sizeof(kNetworkSites[0]);
void* g_networkOriginal[kNetworkHookCount]{};
uintptr_t g_networkBase = 0;
unsigned g_networkActiveMask = 0;
std::atomic<bool> g_networkReady{false};
struct NetworkRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(NetworkRegisters) == 136);
}

extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpNetworkPersonDispatch(NetworkRegisters* r)
{
    const size_t index = r->resume;
    if (!g_networkReady.load(std::memory_order_acquire)) {
        r->resume = reinterpret_cast<uintptr_t>(g_networkOriginal[index]); return;
    }
    if (index == 0) {
        r->rbx = FirstNetworkNode(r->rbp - 0x270, r->rbp - 0x380);
        r->resume = g_networkBase + 0x2e7652a;
    } else if (index == 1) {
        r->rbx = NextNetworkNode(r->rbp - 0x270, r->rbx);
        // The original TEST runs from the trampoline, retaining exact flags.
        r->resume = reinterpret_cast<uintptr_t>(g_networkOriginal[1]) + 3;
    } else if (index == 3) {
        // Restore paths for affected people in the same insertion/rehash order
        // as Windows. This first collection precedes the network collection.
        r->rax = FirstNetworkNode(r->rbp - 0x2b0, r->rbp - 0x3c0);
        r->resume = g_networkBase + 0x2e73f7f;
    } else if (index == 4) {
        r->rax = NextNetworkNode(r->rbp - 0x2b0, r->rax);
        // Replay the stack store; the following TEST remains original.
        r->resume = reinterpret_cast<uintptr_t>(g_networkOriginal[4]) + 3;
    } else {
        // This destructor also runs from the compiler's constructor-unwind
        // cleanup path. All other unordered_set instances remain untracked.
        ForgetNetworkSet(r->rdi);
        r->resume = reinterpret_cast<uintptr_t>(g_networkOriginal[2]);
    }
}

extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpNetworkPersonEntry()
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
        "stmxcsr 256(%rsp)\n\tmov %rbx, %rdi\n\tcall Tpf2mpNetworkPersonDispatch\n\tldmxcsr 256(%rsp)\n\t"
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
#define NETWORK_STUB(index) \
    __attribute__((naked, noinline)) void NetworkStub##index() { \
        __asm__("push $" #index "\n\tjmp Tpf2mpNetworkPersonEntry\n\t"); \
    }
NETWORK_STUB(0) NETWORK_STUB(1) NETWORK_STUB(2) NETWORK_STUB(3) NETWORK_STUB(4)
#undef NETWORK_STUB
void* const kNetworkDetours[] = {
    reinterpret_cast<void*>(NetworkStub0), reinterpret_cast<void*>(NetworkStub1),
    reinterpret_cast<void*>(NetworkStub2), reinterpret_cast<void*>(NetworkStub3),
    reinterpret_cast<void*>(NetworkStub4)
};
using NetworkHooker = bool (*)(uintptr_t, void*, int, void**);
using NetworkWriter = int (*)(uintptr_t, const uint8_t*, size_t, int*);
bool InstallNetworkOrder(uintptr_t base, const char* buildId, NetworkHooker hook, NetworkWriter write)
{
    if (g_networkActiveMask) return false;
    if (!base || !buildId || std::strcmp(buildId, kNetworkBuildId)) {
        g_networkStatus.store("off (unverified image)"); return false;
    }
    for (const auto& context : kNetworkContexts) {
        if (!std::memcmp(reinterpret_cast<void*>(base + context.rva), context.bytes, context.size)) continue;
        g_networkStatus.store("off (unverified affected-network collection/traversal/cleanup)"); return false;
    }
    g_networkReady.store(false, std::memory_order_release);
    g_networkBase = base;
    for (size_t i = 0; i < kNetworkHookCount; ++i) {
        const auto& site = kNetworkSites[i];
        if (hook(base + site.rva, kNetworkDetours[i], site.length, &g_networkOriginal[i])) {
            g_networkActiveMask |= 1u << i; continue;
        }
        g_networkActiveMask = 0;
        for (size_t j = 0; j <= i; ++j) {
            const auto& old = kNetworkSites[j]; int error = 0;
            write(base + old.rva, old.bytes, old.length, &error);
            if (std::memcmp(reinterpret_cast<void*>(base + old.rva), old.bytes, old.length))
                g_networkActiveMask |= 1u << j;
        }
        g_networkStatus.store(g_networkActiveMask ? "ERROR: affected-network hook rollback incomplete"
            : "off (affected-network installation failed; original bytes restored)");
        return false;
    }
    g_networkReady.store(true, std::memory_order_release);
    g_networkStatus.store("enabled (Windows order for temporary affected-person and network entities)"); return true;
}
}
bool Tpf2mpInstallNetworkPersonOrder(uintptr_t imageBase, const char* buildId)
{
    return InstallNetworkOrder(imageBase, buildId, InstallHook, Tpf2mpCodeWriteSelf);
}
const char* Tpf2mpNetworkPersonOrderStatus() { return g_networkStatus.load(std::memory_order_relaxed); }
void Tpf2mpNetworkPersonOrderSetLog(Tpf2mpNetworkPersonLog log)
{
    g_networkLog.store(log, std::memory_order_relaxed);
}
