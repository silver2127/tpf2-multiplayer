#include "building_order_linux.h"
#include "hook.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace {
struct BuildingLess {
    void* context;
    Tpf2mpBuildingOrderLookup lookup;
    bool operator()(int32_t a, int32_t b) const {
        const auto x = lookup(context, a), y = lookup(context, b);
        return x.priority < y.priority || (x.priority == y.priority && x.year < y.year);
    }
};
void OrderThree(int32_t* first, int32_t* middle, int32_t* last, BuildingLess less)
{
    if (less(*middle, *first)) std::swap(*middle, *first);
    if (less(*last, *middle)) {
        std::swap(*last, *middle);
        if (less(*middle, *first)) std::swap(*middle, *first);
    }
}
std::pair<int32_t*, int32_t*> BuildingPartition(int32_t* first, int32_t* last, BuildingLess less)
{
    int32_t* middle = first + (last - first) / 2;
    int32_t* final = last - 1;
    if (final - first > 40) {
        const auto step = (final - first + 1) / 8;
        OrderThree(first, first + step, first + 2 * step, less);
        OrderThree(middle - step, middle, middle + step, less);
        OrderThree(final - 2 * step, final - step, final, less);
        OrderThree(first + step, middle, final - step, less);
    } else OrderThree(first, middle, final, less);
    int32_t* pivotFirst = middle;
    int32_t* pivotLast = middle + 1;
    while (first < pivotFirst && !less(pivotFirst[-1], *pivotFirst)
                              && !less(*pivotFirst, pivotFirst[-1])) --pivotFirst;
    while (pivotLast < last && !less(*pivotLast, *pivotFirst)
                            && !less(*pivotFirst, *pivotLast)) ++pivotLast;
    int32_t* greaterFirst = pivotLast;
    int32_t* greaterLast = pivotFirst;
    for (;;) {
        for (; greaterFirst < last; ++greaterFirst) {
            if (less(*pivotFirst, *greaterFirst)) continue;
            if (less(*greaterFirst, *pivotFirst)) break;
            if (pivotLast != greaterFirst) std::swap(*pivotLast, *greaterFirst);
            ++pivotLast;
        }
        for (; first < greaterLast; --greaterLast) {
            if (less(greaterLast[-1], *pivotFirst)) continue;
            if (less(*pivotFirst, greaterLast[-1])) break;
            --pivotFirst;
            if (pivotFirst != greaterLast - 1) std::swap(*pivotFirst, greaterLast[-1]);
        }
        if (greaterLast == first && greaterFirst == last) return {pivotFirst, pivotLast};
        if (greaterLast == first) {
            if (pivotLast != greaterFirst) std::swap(*pivotFirst, *pivotLast);
            ++pivotLast;
            std::swap(*pivotFirst++, *greaterFirst++);
        } else if (greaterFirst == last) {
            if (--greaterLast != --pivotFirst) std::swap(*greaterLast, *pivotFirst);
            std::swap(*pivotFirst, *--pivotLast);
        } else std::swap(*greaterFirst++, *--greaterLast);
    }
}
void BuildingSift(int32_t* first, size_t hole, size_t length, int32_t value, BuildingLess less)
{
    const size_t top = hole;
    const size_t finalParent = (length - 1) / 2;
    while (hole < finalParent) {
        size_t child = hole * 2 + 2;
        if (less(first[child], first[child - 1])) --child;
        first[hole] = first[child]; hole = child;
    }
    if (hole == finalParent && !(length & 1)) {
        first[hole] = first[length - 1]; hole = length - 1;
    }
    while (top < hole) {
        const size_t parent = (hole - 1) / 2;
        if (!less(first[parent], value)) break;
        first[hole] = first[parent]; hole = parent;
    }
    first[hole] = value;
}
void BuildingHeapSort(int32_t* first, int32_t* last, BuildingLess less)
{
    size_t length = size_t(last - first);
    for (size_t parent = length / 2; parent;)
        { --parent; BuildingSift(first, parent, length, first[parent], less); }
    while (length > 1) {
        const int32_t value = first[length - 1];
        first[length - 1] = first[0]; --length;
        BuildingSift(first, 0, length, value, less);
    }
}
void WindowsBuildingSort(int32_t* first, int32_t* last, size_t ideal, BuildingLess less)
{
    // Original Windows sort at 2457590: 32-element insertion threshold,
    // partition at 2457060 and a 3/4 budget, recursing on the smaller side.
    while (last - first > 32 && ideal) {
        const auto middle = BuildingPartition(first, last, less);
        ideal = ideal / 2 + ideal / 4;
        if (middle.first - first < last - middle.second) {
            WindowsBuildingSort(first, middle.first, ideal, less); first = middle.second;
        } else {
            WindowsBuildingSort(middle.second, last, ideal, less); last = middle.first;
        }
    }
    if (last - first > 32) { BuildingHeapSort(first, last, less); return; }
    for (int32_t* current = first + (first != last); current != last; ++current) {
        const int32_t value = *current;
        int32_t* hole = current;
        while (first != hole && less(value, hole[-1])) { *hole = hole[-1]; --hole; }
        *hole = value;
    }
}
}
void Tpf2mpWindowsBuildingOrder(int32_t* ids, size_t count, void* context,
                               Tpf2mpBuildingOrderLookup lookup)
{
    if (count < 2) return;
    // Unique construction IDs recover the order before the native unstable
    // sort. This does not impose a filename/locale tie-break absent on Windows.
    std::sort(ids, ids + count);
    WindowsBuildingSort(ids, ids + count, count, {context, lookup});
}

namespace {
constexpr char kBuildingBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
constexpr uintptr_t kBuildingPatchRva = 0x1868223;
constexpr uintptr_t kBuildingResumeRva = 0x186822a;
#include "building_order_sites_linux.h"
static_assert(kBuildingResumeRva - kBuildingPatchRva == 7);
struct BuildingRegisters {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags;
};
static_assert(sizeof(BuildingRegisters) == 128);
static_assert(offsetof(BuildingRegisters, r12) == 24);
static_assert(offsetof(BuildingRegisters, rbp) == 32);
bool g_buildingInstalled = false;
const char* g_buildingStatus = "off (not initialized)";

// ConstructionRep's original getter uses the same dense vector, payload
// pointer and two signed comparator fields. No game callback or allocation.
Tpf2mpBuildingOrderKey NativeBuildingKey(void* context, int32_t id)
{
    const auto* records = static_cast<const unsigned char*>(context);
    const unsigned char* payload;
    Tpf2mpBuildingOrderKey key;
    std::memcpy(&payload, records + size_t(id) * 48 + 0x20, 8);
    std::memcpy(&key.priority, payload + 0x1f8, 4);
    std::memcpy(&key.year, payload + 0x28, 4);
    return key;
}
}
extern "C" {
__attribute__((visibility("hidden"))) void* Tpf2mpOriginalBuildingOrder = nullptr;
}
extern "C" __attribute__((visibility("hidden"), noinline))
void Tpf2mpBuildingOrderDispatch(BuildingRegisters* registers)
{
    // Source list is still private to the constructor. Reorder it before any
    // BuildingTypeRep indices, name maps or metadata records are registered.
    uintptr_t list[3], resources[2];
    std::memcpy(list, reinterpret_cast<const void*>(registers->rbp - 0x130), sizeof(list));
    std::memcpy(resources, reinterpret_cast<const void*>(registers->r12 + 0x28), sizeof(resources));
    if (list[1] < list[0] || list[2] < list[1] || (list[1] - list[0]) % 4 ||
        resources[1] < resources[0] || (resources[1] - resources[0]) % 48) {
        g_buildingStatus = "ERROR: installed; unexpected construction vector layout"; return;
    }
    const size_t count = (list[1] - list[0]) / 4;
    const size_t resourceCount = (resources[1] - resources[0]) / 48;
    auto* ids = reinterpret_cast<int32_t*>(list[0]);
    for (size_t i = 0; i < count; ++i) {
        uintptr_t payload = 0;
        if (ids[i] < 0 || size_t(ids[i]) >= resourceCount) {
            g_buildingStatus = "ERROR: installed; unexpected construction ID"; return;
        }
        std::memcpy(&payload, reinterpret_cast<const void*>(resources[0] + size_t(ids[i]) * 48 + 0x20), 8);
        if (!payload) { g_buildingStatus = "ERROR: installed; missing construction payload"; return; }
    }
    Tpf2mpWindowsBuildingOrder(ids, count, reinterpret_cast<void*>(resources[0]), NativeBuildingKey);
}
namespace {
// Mid-function jump: preserve every GP/XMM register and entry flags. The
// seven-byte trampoline replays MOV RDI,[RBP-0x110], which has no RIP operand.
__attribute__((naked, noinline)) void BuildingOrderEntry()
{
    __asm__(
        "pushfq\n\t"
        "push %rax\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov %rsp, %rbx\n\t"
        "and $-16, %rsp\n\t"
        "sub $256, %rsp\n\t"
        "movdqu %xmm0, 0(%rsp)\n\t"
        "movdqu %xmm1, 16(%rsp)\n\t"
        "movdqu %xmm2, 32(%rsp)\n\t"
        "movdqu %xmm3, 48(%rsp)\n\t"
        "movdqu %xmm4, 64(%rsp)\n\t"
        "movdqu %xmm5, 80(%rsp)\n\t"
        "movdqu %xmm6, 96(%rsp)\n\t"
        "movdqu %xmm7, 112(%rsp)\n\t"
        "movdqu %xmm8, 128(%rsp)\n\t"
        "movdqu %xmm9, 144(%rsp)\n\t"
        "movdqu %xmm10, 160(%rsp)\n\t"
        "movdqu %xmm11, 176(%rsp)\n\t"
        "movdqu %xmm12, 192(%rsp)\n\t"
        "movdqu %xmm13, 208(%rsp)\n\t"
        "movdqu %xmm14, 224(%rsp)\n\t"
        "movdqu %xmm15, 240(%rsp)\n\t"
        "mov %rbx, %rdi\n\t"
        "call Tpf2mpBuildingOrderDispatch\n\t"
        "movdqu 0(%rsp), %xmm0\n\t"
        "movdqu 16(%rsp), %xmm1\n\t"
        "movdqu 32(%rsp), %xmm2\n\t"
        "movdqu 48(%rsp), %xmm3\n\t"
        "movdqu 64(%rsp), %xmm4\n\t"
        "movdqu 80(%rsp), %xmm5\n\t"
        "movdqu 96(%rsp), %xmm6\n\t"
        "movdqu 112(%rsp), %xmm7\n\t"
        "movdqu 128(%rsp), %xmm8\n\t"
        "movdqu 144(%rsp), %xmm9\n\t"
        "movdqu 160(%rsp), %xmm10\n\t"
        "movdqu 176(%rsp), %xmm11\n\t"
        "movdqu 192(%rsp), %xmm12\n\t"
        "movdqu 208(%rsp), %xmm13\n\t"
        "movdqu 224(%rsp), %xmm14\n\t"
        "movdqu 240(%rsp), %xmm15\n\t"
        "mov %rbx, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rax\n\t"
        "popfq\n\t"
        "jmp *Tpf2mpOriginalBuildingOrder(%rip)\n\t"
    );
}
}
bool Tpf2mpInstallBuildingOrder(uintptr_t imageBase, const char* buildId)
{
    if (g_buildingInstalled) { return false; }
    if (!imageBase || !buildId || std::strcmp(buildId, kBuildingBuildId)) {
        g_buildingStatus = "off (unverified image)"; return false;
    }
    struct Context { uintptr_t rva; const unsigned char* bytes; size_t size; };
    const Context contexts[] = {
        {kBuildingConstructorRva, kBuildingConstructorBytes, sizeof(kBuildingConstructorBytes)},
        {kBuildingGetTypesRva, kBuildingGetTypesBytes, sizeof(kBuildingGetTypesBytes)},
        {kBuildingComparatorRva, kBuildingComparatorBytes, sizeof(kBuildingComparatorBytes)},
    };
    for (const auto& context : contexts)
        if (std::memcmp(reinterpret_cast<void*>(imageBase + context.rva), context.bytes, context.size)) {
            g_buildingStatus = "off (unverified construction registration/comparator)"; return false;
        }
    g_buildingInstalled = InstallHook(imageBase + kBuildingPatchRva,
        reinterpret_cast<void*>(BuildingOrderEntry), 7, &Tpf2mpOriginalBuildingOrder);
    if (g_buildingInstalled)
        g_buildingStatus = "enabled (Windows ordering before building registration)";
    else if (!std::memcmp(reinterpret_cast<void*>(imageBase + kBuildingPatchRva),
                         kBuildingConstructorBytes + kBuildingPatchRva - kBuildingConstructorRva, 7))
        g_buildingStatus = "off (hook failed; original registration intact)";
    else g_buildingStatus = "ERROR: hook failed and registration instruction is altered";
    return g_buildingInstalled;
}
const char* Tpf2mpBuildingOrderStatus() { return g_buildingStatus; }
