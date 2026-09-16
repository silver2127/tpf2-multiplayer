#include "network_index_order_linux.h"
#include "windows_entity_set_order_linux.h"
#include "hook.h"
#include "codewrite_linux.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace {
struct NetworkIndexRecord {
    NetworkIndexRecord* next;
    uint64_t pair;
    uintptr_t nativeSet;
    bool invalid;
    Tpf2mpWindowsEntitySet order;
};
struct NetworkIndexOwner { NetworkIndexOwner* next; uintptr_t map; bool invalid; NetworkIndexRecord* records; };
pthread_mutex_t g_networkIndexMutex = PTHREAD_MUTEX_INITIALIZER;
NetworkIndexOwner* g_networkIndexOwners = nullptr;
std::atomic<const char*> g_networkIndexStatus{"off (not initialized)"};
std::atomic<Tpf2mpNetworkIndexOrderLog> g_networkIndexLogger{nullptr};
std::atomic<unsigned> g_networkIndexReportedErrors{0};
void NetworkIndexError(const char* message) noexcept
{
    g_networkIndexStatus.store(message, std::memory_order_relaxed);
    const auto log = g_networkIndexLogger.load(std::memory_order_relaxed);
    if (!log) return;
    const char* failures[] = {
        "ERROR: network-index owner reused without verified cleanup",
        "ERROR: network-index initialization capture failed",
        "ERROR: insertion into an untracked network-index owner",
        "ERROR: network-index set history allocation failed",
        "ERROR: incomplete network-index set insertion history",
        "ERROR: erase from an untracked network-index owner",
        "ERROR: missing network-index set erase history",
        "ERROR: inconsistent network-index set erase history",
        "ERROR: incomplete network-index set capture; native traversal retained",
        "ERROR: untracked nonempty network-index set traversal"
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        if (std::strcmp(message, failures[i])) continue;
        const unsigned bit = 1u << i;
        if (!(g_networkIndexReportedErrors.fetch_or(bit, std::memory_order_relaxed) & bit))
            log("[network-index] %s\n", message);
        break;
    }
}
uintptr_t NetworkIndexRead(uintptr_t address) noexcept { uintptr_t value; std::memcpy(&value, reinterpret_cast<void*>(address), 8); return value; }
uint32_t NetworkIndexKey(uintptr_t node) noexcept { uint32_t value; std::memcpy(&value, reinterpret_cast<void*>(node + 8), 4); return value; }
void DeleteNetworkIndexRecord(NetworkIndexRecord* record) noexcept { Tpf2mpWindowsEntitySetClear(record->order); std::free(record); }
void DeleteNetworkIndexOwner(NetworkIndexOwner* owner) noexcept
{
    for (auto* record = owner->records; record;) { auto* next = record->next; DeleteNetworkIndexRecord(record); record = next; }
    std::free(owner);
}
void ForgetNetworkIndexOwner(uintptr_t map) noexcept
{
    pthread_mutex_lock(&g_networkIndexMutex);
    for (auto** at = &g_networkIndexOwners; *at; at = &(*at)->next) if ((*at)->map == map) {
        auto* old = *at; *at = old->next; DeleteNetworkIndexOwner(old); break;
    }
    pthread_mutex_unlock(&g_networkIndexMutex);
}
void BindNetworkIndexOwner(uintptr_t map) noexcept
{
    pthread_mutex_lock(&g_networkIndexMutex);
    for (auto** at = &g_networkIndexOwners; *at; at = &(*at)->next) if ((*at)->map == map) {
        auto* old = *at; *at = old->next; DeleteNetworkIndexOwner(old);
        NetworkIndexError("ERROR: network-index owner reused without verified cleanup"); break;
    }
    const bool empty = !NetworkIndexRead(map + 0x10) && !NetworkIndexRead(map + 0x18);
    auto* owner = empty ? static_cast<NetworkIndexOwner*>(std::calloc(1, sizeof(NetworkIndexOwner))) : nullptr;
    if (owner) { owner->map = map; owner->next = g_networkIndexOwners; g_networkIndexOwners = owner; }
    else NetworkIndexError("ERROR: network-index initialization capture failed");
    pthread_mutex_unlock(&g_networkIndexMutex);
}
uintptr_t LookupNetworkIndexSet(uintptr_t map, uint64_t pair) noexcept
{
    const size_t count = NetworkIndexRead(map + 0x18);
    size_t visited = 0;
    for (uintptr_t node = NetworkIndexRead(map + 0x10); node && visited++ < count; node = NetworkIndexRead(node))
        if (NetworkIndexRead(node + 8) == pair) return node + 0x10;
    return 0;
}
void ObserveNetworkIndexInsert(uintptr_t map, uint64_t pair, uint32_t id, uintptr_t set) noexcept
{
    uintptr_t inserted = 0;
    size_t visited = 0;
    const size_t count = set ? NetworkIndexRead(set + 0x18) : 0;
    for (uintptr_t node = set ? NetworkIndexRead(set + 0x10) : 0; node && visited++ < count; node = NetworkIndexRead(node))
        if (NetworkIndexKey(node) == id) { inserted = node; break; }
    pthread_mutex_lock(&g_networkIndexMutex);
    NetworkIndexOwner* owner = g_networkIndexOwners;
    while (owner && owner->map != map) owner = owner->next;
    if (!owner) NetworkIndexError("ERROR: insertion into an untracked network-index owner");
    else if (!owner->invalid) {
        NetworkIndexRecord* record = owner->records;
        while (record && record->pair != pair) record = record->next;
        if (!record) {
            record = static_cast<NetworkIndexRecord*>(std::calloc(1, sizeof(NetworkIndexRecord)));
            if (record) { record->pair = pair; record->nativeSet = set; record->next = owner->records; owner->records = record; }
            else { owner->invalid = true; NetworkIndexError("ERROR: network-index set history allocation failed"); }
        }
        if (record && !record->invalid && (!inserted || record->nativeSet != set ||
            count != record->order.count + (Tpf2mpWindowsEntitySetFind(record->order, id) ? 0 : 1) ||
            !Tpf2mpWindowsEntitySetInsert(record->order, id, inserted))) {
            record->invalid = true; NetworkIndexError("ERROR: incomplete network-index set insertion history");
        }
    }
    pthread_mutex_unlock(&g_networkIndexMutex);
}
void ObserveNetworkIndexErase(uintptr_t map, uint64_t pair, uint32_t id) noexcept
{
    const uintptr_t set = LookupNetworkIndexSet(map, pair);
    const size_t count = set ? NetworkIndexRead(set + 0x18) : 0;
    pthread_mutex_lock(&g_networkIndexMutex);
    NetworkIndexOwner* owner = g_networkIndexOwners;
    while (owner && owner->map != map) owner = owner->next;
    if (!owner) NetworkIndexError("ERROR: erase from an untracked network-index owner");
    else if (!owner->invalid) {
        auto* record = owner->records;
        while (record && record->pair != pair) record = record->next;
        if (!record) {
            if (count) { owner->invalid = true; NetworkIndexError("ERROR: missing network-index set erase history"); }
        } else if (!record->invalid) {
            if (Tpf2mpWindowsEntitySetFind(record->order, id)) Tpf2mpWindowsEntitySetErase(record->order, id);
            if (set != record->nativeSet || count != record->order.count) {
                record->invalid = true; NetworkIndexError("ERROR: inconsistent network-index set erase history");
            }
        }
    }
    pthread_mutex_unlock(&g_networkIndexMutex);
}

bool ValidateNetworkIndexRecord(NetworkIndexRecord& record) noexcept
{
    if (record.invalid) return false;
    const size_t count = NetworkIndexRead(record.nativeSet + 0x18);
    if (count != record.order.count) record.invalid = true;
    size_t visited = 0;
    for (uintptr_t node = NetworkIndexRead(record.nativeSet + 0x10); !record.invalid && node; node = NetworkIndexRead(node)) {
        auto* found = Tpf2mpWindowsEntitySetFind(record.order, NetworkIndexKey(node));
        if (++visited > count || !found || found->nativeNode != node) record.invalid = true;
    }
    if (visited != count) record.invalid = true;
    if (record.invalid) NetworkIndexError("ERROR: incomplete network-index set capture; native traversal retained");
    return !record.invalid;
}
uintptr_t FirstNetworkIndexNode(uintptr_t set) noexcept
{
    uintptr_t result = NetworkIndexRead(set + 0x10);
    if (!result) return 0;
    bool found = false;
    pthread_mutex_lock(&g_networkIndexMutex);
    for (auto* owner = g_networkIndexOwners; owner; owner = owner->next)
        for (auto* record = owner->records; record; record = record->next)
            if (record->nativeSet == set) {
                found = true;
                if (!owner->invalid && ValidateNetworkIndexRecord(*record))
                    result = record->order.first ? record->order.first->nativeNode : 0;
            }
    if (!found) NetworkIndexError("ERROR: untracked nonempty network-index set traversal");
    pthread_mutex_unlock(&g_networkIndexMutex); return result;
}
uintptr_t NextNetworkIndexNode(uintptr_t node) noexcept
{
    uintptr_t result = NetworkIndexRead(node);
    const uint32_t id = NetworkIndexKey(node);
    pthread_mutex_lock(&g_networkIndexMutex);
    for (auto* owner = g_networkIndexOwners; owner; owner = owner->next) if (!owner->invalid)
        for (auto* record = owner->records; record; record = record->next) if (!record->invalid) {
            auto* found = Tpf2mpWindowsEntitySetFind(record->order, id);
            if (found && found->nativeNode == node) result = found->next ? found->next->nativeNode : 0;
        }
    pthread_mutex_unlock(&g_networkIndexMutex); return result;
}

constexpr char kIndexBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
struct IndexContext { uintptr_t rva; const unsigned char* bytes; size_t size; };
struct IndexSite { uintptr_t rva; unsigned length; unsigned char bytes[16]; };
#include "network_index_order_sites_linux.h"
void* g_indexOriginal[10]{};
uintptr_t g_indexBase = 0;
unsigned g_indexActiveMask = 0;
std::atomic<bool> g_indexReady{false};
struct IndexRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags, resume;
};
static_assert(sizeof(IndexRegisters) == 136);
}
extern "C" {
__attribute__((visibility("hidden"))) uintptr_t Tpf2mpNetworkIndexLoop[4]{};
__attribute__((visibility("hidden"))) uintptr_t Tpf2mpNetworkIndexFallthrough[4]{};
}
namespace {
#define INDEX_BRANCH(index,offset) \
    __attribute__((naked,noinline)) void IndexBranch##index() { \
        __asm__("cmp %rbx,%r14\n\tje 1f\n\tjmp *Tpf2mpNetworkIndexLoop+" #offset "(%rip)\n\t1: jmp *Tpf2mpNetworkIndexFallthrough+" #offset "(%rip)\n\t"); \
    }
INDEX_BRANCH(0,0) INDEX_BRANCH(1,8) INDEX_BRANCH(2,16) INDEX_BRANCH(3,24)
#undef INDEX_BRANCH
void* const kIndexBranches[]={reinterpret_cast<void*>(IndexBranch0),reinterpret_cast<void*>(IndexBranch1),reinterpret_cast<void*>(IndexBranch2),reinterpret_cast<void*>(IndexBranch3)};
}
extern "C" __attribute__((visibility("hidden"),noinline))
void Tpf2mpNetworkIndexDispatch(IndexRegisters* r)
{
    const size_t index = r->resume;
    // Original short loop branches cannot be copied into a remote trampoline.
    // Their assembly continuations replay CMP/JNE exactly, including on rollback.
    if (index >= 2 && index <= 5) {
        r->resume = reinterpret_cast<uintptr_t>(kIndexBranches[index - 2]);
        if (!g_indexReady.load(std::memory_order_acquire)) return;
        const uint64_t pair = NetworkIndexRead(r->rbp - 0x40);
        const uint32_t id = NetworkIndexKey(r->r12 - 8);
        if (index <= 3) ObserveNetworkIndexInsert(NetworkIndexRead(r->rbp - 0x58), pair, id, NetworkIndexRead(r->rbp - 0x48));
        else ObserveNetworkIndexErase(r->r15, pair, id);
        return;
    }
    r->resume = reinterpret_cast<uintptr_t>(g_indexOriginal[index]);
    if (!g_indexReady.load(std::memory_order_acquire)) return;
    if (index == 0) {
        const uint64_t zero = 0; std::memcpy(reinterpret_cast<void*>(r->rdi + 0xc0), &zero, 8);
        BindNetworkIndexOwner(r->rdi + 0x90); r->resume = g_indexBase + 0x170c2fe;
    } else if (index == 1) ForgetNetworkIndexOwner(r->rdi);
    else if (index == 6) {
        r->r12 = FirstNetworkIndexNode(r->r12 + 0x10); r->resume = g_indexBase + 0x170e1fe;
    } else {
        r->r12 = NextNetworkIndexNode(r->r12);
        r->resume = reinterpret_cast<uintptr_t>(g_indexOriginal[index]) + 4;
    }
}
extern "C" __attribute__((visibility("hidden"), naked, noinline))
void Tpf2mpNetworkIndexEntry()
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
        "stmxcsr 256(%rsp)\n\tmov %rbx, %rdi\n\tcall Tpf2mpNetworkIndexDispatch\n\tldmxcsr 256(%rsp)\n\t"
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
#define INDEX_STUB(index) \
    __attribute__((naked,noinline)) void IndexStub##index() { __asm__("push $" #index "\n\tjmp Tpf2mpNetworkIndexEntry\n\t"); }
INDEX_STUB(0) INDEX_STUB(1) INDEX_STUB(2) INDEX_STUB(3) INDEX_STUB(4)
INDEX_STUB(5) INDEX_STUB(6) INDEX_STUB(7) INDEX_STUB(8) INDEX_STUB(9)
#undef INDEX_STUB
void* const kIndexDetours[]={reinterpret_cast<void*>(IndexStub0),reinterpret_cast<void*>(IndexStub1),reinterpret_cast<void*>(IndexStub2),reinterpret_cast<void*>(IndexStub3),reinterpret_cast<void*>(IndexStub4),reinterpret_cast<void*>(IndexStub5),reinterpret_cast<void*>(IndexStub6),reinterpret_cast<void*>(IndexStub7),reinterpret_cast<void*>(IndexStub8),reinterpret_cast<void*>(IndexStub9)};
using IndexHooker=bool(*)(uintptr_t,void*,int,void**);
using IndexWriter=int(*)(uintptr_t,const uint8_t*,size_t,int*);
bool InstallIndexOrder(uintptr_t base,const char* buildId,IndexHooker hook,IndexWriter write)
{
    if(g_indexActiveMask)return false;
    if(!base||!buildId||std::strcmp(buildId,kIndexBuildId)){NetworkIndexError("off (unverified image)");return false;}
    for(const auto& c:kIndexContexts)if(std::memcmp(reinterpret_cast<void*>(base+c.rva),c.bytes,c.size)){
        NetworkIndexError("off (unverified network-index lifetime/writers/producer)");return false;
    }
    g_indexBase=base;g_indexReady.store(false,std::memory_order_release);
    const uintptr_t loops[]={0x170d8a0,0x170ef88,0x170e7f8,0x170e950};
    const uintptr_t falls[]={0x170d8d3,0x170efbb,0x170e81e,0x170e976};
    for(size_t i=0;i<4;++i){Tpf2mpNetworkIndexLoop[i]=base+loops[i];Tpf2mpNetworkIndexFallthrough[i]=base+falls[i];}
    for(size_t i=0;i<10;++i){
        const auto& site=kIndexSites[i];
        if(hook(base+site.rva,kIndexDetours[i],site.length,&g_indexOriginal[i])){g_indexActiveMask|=1u<<i;continue;}
        g_indexActiveMask=0;
        for(size_t j=0;j<=i;++j){const auto& old=kIndexSites[j];int error=0;write(base+old.rva,old.bytes,old.length,&error);if(std::memcmp(reinterpret_cast<void*>(base+old.rva),old.bytes,old.length))g_indexActiveMask|=1u<<j;}
        NetworkIndexError(g_indexActiveMask?"ERROR: network-index hook rollback incomplete":"off (network-index installation failed; original bytes restored)");return false;
    }
    g_indexReady.store(true,std::memory_order_release);
    NetworkIndexError("enabled (Windows order in persistent person-route index)");return true;
}
}
bool Tpf2mpInstallNetworkIndexOrder(uintptr_t imageBase,const char* buildId){return InstallIndexOrder(imageBase,buildId,InstallHook,Tpf2mpCodeWriteSelf);}
const char* Tpf2mpNetworkIndexOrderStatus(){return g_networkIndexStatus.load(std::memory_order_relaxed);}
void Tpf2mpNetworkIndexOrderSetLog(Tpf2mpNetworkIndexOrderLog log){g_networkIndexLogger.store(log,std::memory_order_relaxed);}
