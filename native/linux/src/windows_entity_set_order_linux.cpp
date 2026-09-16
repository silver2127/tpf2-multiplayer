#include "windows_entity_set_order_linux.h"
#include <cstdlib>

namespace {
uint64_t Hash(uint32_t id) noexcept
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (unsigned byte = 0; byte != 4; ++byte) {
        hash = (hash ^ uint8_t(id)) * UINT64_C(0x100000001b3); id >>= 8;
    }
    return hash;
}
void Place(Tpf2mpWindowsEntitySet& set, Tpf2mpWindowsEntitySetNode* node) noexcept
{
    auto& bucket = set.buckets[Hash(node->id) & (set.bucketCount - 1)];
    if (!bucket.first) {
        node->previous = set.last; node->next = nullptr;
        if (set.last) set.last->next = node; else set.first = node;
        set.last = node; bucket.first = bucket.last = node;
    } else {
        node->next = bucket.first; node->previous = bucket.first->previous;
        if (node->previous) node->previous->next = node; else set.first = node;
        bucket.first->previous = node; bucket.first = node;
    }
}
}
Tpf2mpWindowsEntitySetNode* Tpf2mpWindowsEntitySetFind(Tpf2mpWindowsEntitySet& set, uint32_t id) noexcept
{
    if (!set.buckets) return nullptr;
    const auto& bucket = set.buckets[Hash(id) & (set.bucketCount - 1)];
    if (!bucket.first) return nullptr;
    for (auto* node = bucket.first; ; node = node->next) {
        if (node->id == id) return node;
        if (node == bucket.last) return nullptr;
    }
}
void Tpf2mpWindowsEntitySetClear(Tpf2mpWindowsEntitySet& set) noexcept
{
    for (auto* node = set.first; node;) { auto* next = node->next; std::free(node); node = next; }
    std::free(set.buckets); set = {};
}
bool Tpf2mpWindowsEntitySetInsert(Tpf2mpWindowsEntitySet& set, uint32_t id, uintptr_t nativeNode) noexcept
{
    if (auto* old = Tpf2mpWindowsEntitySetFind(set, id)) return old->nativeNode == nativeNode;
    if (!set.buckets) {
        set.buckets = static_cast<Tpf2mpWindowsEntitySetBucket*>(std::calloc(8, sizeof(Tpf2mpWindowsEntitySetBucket)));
        if (!set.buckets) return false;
        set.bucketCount = 8;
    }
    auto* node = static_cast<Tpf2mpWindowsEntitySetNode*>(std::calloc(1, sizeof(Tpf2mpWindowsEntitySetNode)));
    if (!node) return false;
    node->id = id; node->nativeNode = nativeNode; Place(set, node); ++set.count;
    if (set.count > set.bucketCount) {
        const size_t multiplier = set.bucketCount < 512 ? 8 : 2;
        if (set.bucketCount > SIZE_MAX / multiplier / sizeof(Tpf2mpWindowsEntitySetBucket)) return false;
        const size_t count = set.bucketCount * multiplier;
        auto* buckets = static_cast<Tpf2mpWindowsEntitySetBucket*>(std::calloc(count, sizeof(Tpf2mpWindowsEntitySetBucket)));
        if (!buckets) return false;
        std::free(set.buckets); set.buckets = buckets; set.bucketCount = count;
        auto* current = set.first; set.first = set.last = nullptr;
        while (current) { auto* next = current->next; Place(set, current); current = next; }
    }
    return true;
}
bool Tpf2mpWindowsEntitySetErase(Tpf2mpWindowsEntitySet& set, uint32_t id) noexcept
{
    auto* node = Tpf2mpWindowsEntitySetFind(set, id);
    if (!node) return false;
    auto& bucket = set.buckets[Hash(id) & (set.bucketCount - 1)];
    if (bucket.first == bucket.last) bucket = {};
    else if (bucket.first == node) bucket.first = node->next;
    else if (bucket.last == node) bucket.last = node->previous;
    if (node->previous) node->previous->next = node->next; else set.first = node->next;
    if (node->next) node->next->previous = node->previous; else set.last = node->previous;
    std::free(node); --set.count;
    // The original Windows erase-all path calls clear(), restoring the
    // initial eight buckets. A later insertion must not retain an old size.
    if (!set.count) Tpf2mpWindowsEntitySetClear(set);
    return true;
}
