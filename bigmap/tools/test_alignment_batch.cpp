// The alignment batch detour against the real MSVC std::set layout, and the
// hook's byte anchors against the real executable. No game is touched.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#include <random>
#include <cassert>
#include "../src/tpf2mp_plugin.h"
static const Tpf2mpHost* H = nullptr;
static bool g_gog = false;
#include "../src/alignment_batch.h"

// The game's entry: a block coordinate and a vector of 16-byte items, the
// same shape std::set<Entry> gives in the MSVC STL (value at +0x20, the
// vector's three pointers at +0x28, node 0x40 bytes).
struct Item { uint64_t a, b; };
struct Entry {
    uint64_t key; std::vector<Item> items;
    bool operator<(const Entry& o) const { return key < o.key; }
};
static_assert(sizeof(Entry) == 32, "entry layout");
static uint8_t g_selfBytes[16];                // the system object: its +8 is the CTerrain the detour exposes
static void* const g_self = g_selfBytes;
static std::vector<std::vector<uint64_t>> g_calls;
static void __fastcall Recorder(void* self, AlignmentBatch::SetObject* set) {
    // Iterate the way the game does: from head->left with the MSVC successor
    // step, and read each entry's vector through the copied pointers.
    assert(self == g_self);
    std::vector<uint64_t> keys;
    AlignmentBatch::SetNode* head = set->head;
    for (auto* it = head->left; it != head; it = AlignmentBatch::Next(it)) {
        keys.push_back(it->value.key);
        auto* first = static_cast<Item*>(it->value.vfirst); auto* last = static_cast<Item*>(it->value.vlast);
        assert(size_t(last - first) == it->value.key % 5);              // the vector the test built for this key
        for (auto* p = first; p != last; ++p) assert(p->a == it->value.key && p->b == uint64_t(p - first));
    }
    assert(keys.size() == set->size);
    g_calls.push_back(keys);
}

int main() {
    std::set<Entry> s;
    std::mt19937_64 rng(2026);
    for (int i = 0; i < 10007; ++i) {
        Entry e; e.key = rng();
        for (uint64_t k = 0; k < e.key % 5; ++k) e.items.push_back({e.key, k});
        s.insert(std::move(e));
    }
    auto* obj = reinterpret_cast<AlignmentBatch::SetObject*>(&s);
    assert(obj->size == s.size());
    std::vector<uint64_t> expected; for (auto& e : s) expected.push_back(e.key);
    std::vector<AlignmentBatch::SetValue> walked(s.size());
    assert(AlignmentBatch::CollectValues(obj, walked.data(), walked.size()) == s.size());
    for (size_t i = 0; i < walked.size(); ++i) assert(walked[i].key == expected[i]);
    // Batches of 100: 101 calls, each a chain the successor step visits in order; concatenation equals the set.
    auto self = g_self;
    *reinterpret_cast<void**>(g_selfBytes + 8) = reinterpret_cast<void*>(0x5678);
    g_calls.clear();
    BigmapTestAlignmentDetour(self, obj, Recorder, 100);
    assert(g_alignmentTerrain == reinterpret_cast<void*>(0x5678));   // the pass's CTerrain, for the sidecar
    assert(g_alignmentPassMs >= 0);
    assert(g_calls.size() == (s.size() + 99) / 100);
    std::vector<uint64_t> got;
    for (auto& c : g_calls) { assert(c.size() <= 100); got.insert(got.end(), c.begin(), c.end()); }
    assert(got == expected);
    // A set at or under the batch size, and batch 0, go through untouched.
    g_calls.clear(); BigmapTestAlignmentDetour(self, obj, Recorder, 20000);
    assert(g_calls.size() == 1 && g_calls[0] == expected);
    g_calls.clear(); BigmapTestAlignmentDetour(self, obj, Recorder, 0);
    assert(g_calls.size() == 1 && g_calls[0] == expected);
    // A single-element chain and an exact multiple.
    std::set<Entry> one; one.insert(Entry{40, {}}); g_calls.clear();
    BigmapTestAlignmentDetour(self, reinterpret_cast<AlignmentBatch::SetObject*>(&one), Recorder, 1);
    assert(g_calls.size() == 1 && g_calls[0] == std::vector<uint64_t>{40});
    std::set<Entry> six; for (uint64_t k : {5, 10, 15, 20, 25, 30}) six.insert(Entry{k, {}}); g_calls.clear();
    BigmapTestAlignmentDetour(self, reinterpret_cast<AlignmentBatch::SetObject*>(&six), Recorder, 3);
    assert(g_calls.size() == 2 && g_calls[0] == (std::vector<uint64_t>{5, 10, 15}) && g_calls[1] == (std::vector<uint64_t>{20, 25, 30}));
    // The game's set is untouched by the batching: still iterates and erases normally.
    { std::vector<uint64_t> again; for (auto& e : s) again.push_back(e.key); assert(again == expected); }
    s.clear();
    // Byte anchors in the real executable.
    FILE* f = nullptr; fopen_s(&f, "C:\\tools\\bin\\TransportFever2.exe", "rb"); assert(f);
    auto check = [&](uintptr_t rva, const uint8_t* bytes, size_t n) {
        // .text: file offset = rva - 0x1000 + 0x400 for this image (section 1 at RVA 0x1000, raw 0x400).
        uint8_t buf[32]; fseek(f, long(rva - 0x1000 + 0x400), SEEK_SET); assert(fread(buf, 1, n, f) == n);
        assert(memcmp(buf, bytes, n) == 0);
    };
    check(AlignmentBatch::kUpdateRva, AlignmentBatch::kUpdateBytes, sizeof AlignmentBatch::kUpdateBytes);
    check(AlignmentBatch::kCallerSiteRva, AlignmentBatch::kCallerSiteBytes, sizeof AlignmentBatch::kCallerSiteBytes);
    check(AlignmentBatch::kStepRva, AlignmentBatch::kStepBytes, sizeof AlignmentBatch::kStepBytes);
    fclose(f);
    printf("PASS: MSVC set walk matches std::set, batches concatenate to the set in order, pass-through cases, chains of 1 and exact multiples, byte anchors\n");
    return 0;
}
