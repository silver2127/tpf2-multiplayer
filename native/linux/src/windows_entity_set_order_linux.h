#pragma once
#include <cstddef>
#include <cstdint>

// Private ordering metadata only. nativeNode is an opaque, non-owned token.
struct Tpf2mpWindowsEntitySetNode {
    Tpf2mpWindowsEntitySetNode* next;
    Tpf2mpWindowsEntitySetNode* previous;
    uintptr_t nativeNode;
    uint32_t id;
};
struct Tpf2mpWindowsEntitySetBucket {
    Tpf2mpWindowsEntitySetNode* first;
    Tpf2mpWindowsEntitySetNode* last;
};
struct Tpf2mpWindowsEntitySet {
    Tpf2mpWindowsEntitySetNode* first;
    Tpf2mpWindowsEntitySetNode* last;
    Tpf2mpWindowsEntitySetBucket* buckets;
    size_t count;
    size_t bucketCount; // zero means the unallocated, empty eight-bucket state
};
Tpf2mpWindowsEntitySetNode* Tpf2mpWindowsEntitySetFind(Tpf2mpWindowsEntitySet& set, uint32_t id) noexcept;
bool Tpf2mpWindowsEntitySetInsert(Tpf2mpWindowsEntitySet& set, uint32_t id, uintptr_t nativeNode) noexcept;
bool Tpf2mpWindowsEntitySetErase(Tpf2mpWindowsEntitySet& set, uint32_t id) noexcept;
void Tpf2mpWindowsEntitySetClear(Tpf2mpWindowsEntitySet& set) noexcept;
