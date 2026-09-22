// The AddTile post-hook (terrain_serve.h) against a synthetic terrain grid in
// the game's layout and a real sidecar file, plus its byte anchors in the real
// executable. No game is touched.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <random>
#include <cassert>
#include <array>
#include <algorithm>
#include <new>
#include "../src/tpf2mp_plugin.h"
static const Tpf2mpHost* H = nullptr;
static bool g_gog = false;
// The plugin couples the hook to the tile arena; the test supplies its own mark.
namespace TerrainPager { static bool SetServed(const void*) { return false; } static bool IsServed(const void*) { return false; } }
static bool (*g_terrainServedCheck)(const void*) = nullptr;
static volatile LONG64 g_terrainServedCopiesSkipped = 0;
static void (*g_alignmentPassDone)() = nullptr;
#include "../src/terrain_sidecar.h"
#include "../src/terrain_serve.h"
using namespace TerrainSidecar;

struct FakeTerrain {
    uint8_t cterrain[0x20];
    std::vector<uint8_t> grid;
    std::vector<std::vector<uint16_t>> caches;
    std::vector<std::array<uint8_t, 0x20>> controls;
    FakeTerrain(int nx, int ny) {
        memset(cterrain, 0, sizeof cterrain);
        grid.assign(0x18 + size_t(nx) * ny * 40, 0);
        *reinterpret_cast<int32_t*>(grid.data() + 8) = nx;
        *reinterpret_cast<int32_t*>(grid.data() + 0xc) = ny;
        caches.resize(size_t(nx) * ny); controls.assign(size_t(nx) * ny, {});
        *reinterpret_cast<uint8_t**>(grid.data() + 0x10) = grid.data() + 0x18;
        *reinterpret_cast<uint8_t**>(cterrain + 0x18) = grid.data();
    }
    uint8_t* record(uint32_t i) { return grid.data() + 0x18 + size_t(i) * 40; }
    TileVector* vec(uint32_t i) { return reinterpret_cast<TileVector*>(controls[i].data() + 0x10); }
    void fill(uint32_t i, uint32_t seed) {
        auto& c = caches[i]; c.resize(Samples);
        std::mt19937 rng(seed); uint16_t h = uint16_t(20000 + rng() % 500);
        for (size_t k = 0; k < Samples; ++k) { h = uint16_t(h + int(rng() % 7) - 3); c[k] = h; }
    }
    // What AddTile does: store the entity, attach the control block, size the vector.
    void addTile(uint32_t i, int entity) {
        caches[i].assign(Samples, 0);
        *reinterpret_cast<int32_t*>(record(i) + 0) = entity;
        *reinterpret_cast<uint8_t**>(record(i) + 8) = controls[i].data() + 0x10;   // shared_ptr: the vector object
        *reinterpret_cast<uint8_t**>(record(i) + 0x10) = controls[i].data();       // ...and its control block
        auto* v = vec(i); v->first = caches[i].data(); v->last = v->end = v->first + Samples;
        *reinterpret_cast<int32_t*>(record(i) + 0x20) += 1;
    }
};
// Entities are handed out in grid order here, 1000 + index, and the fake
// AddTile is what the engine's does with them.
static FakeTerrain* g_live = nullptr;
static int g_addCalls = 0;
static void __fastcall FakeAddTile(void* terrain, int entity, uint64_t, uint64_t) {
    assert(terrain == g_live->cterrain);
    g_live->addTile(uint32_t(entity - 1000), entity); ++g_addCalls;
}
static std::vector<const void*> g_marked;
static bool Mark(const void* first) { g_marked.push_back(first); return true; }
static bool MarkFails(const void*) { return false; }

int main() {
    const int nx = 24, ny = 20;   // 480 records
    const char* path = "test_serve_sidecar.bin";
    // 1. A saved world: 70% of the tiles have a cache; write its sidecar.
    FakeTerrain save(nx, ny);
    std::vector<bool> stored(size_t(nx) * ny, false);
    std::mt19937 pick(7);
    for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) if (pick() % 10 < 7) {
        save.fill(i, i * 3 + 11);
        *reinterpret_cast<int32_t*>(save.record(i) + 0) = int32_t(1000 + i);
        *reinterpret_cast<uint8_t**>(save.record(i) + 8) = save.controls[i].data() + 0x10;
        *reinterpret_cast<uint8_t**>(save.record(i) + 0x10) = save.controls[i].data();
        auto* v = save.vec(i); v->first = save.caches[i].data(); v->last = v->end = v->first + Samples;
        stored[i] = true;
    }
    auto* enc = new BlockCodec::EncodeScratch; uint64_t bytes = 0;
    long written = Write(GridOf(save.cterrain), 0x5EED, path, enc, &bytes);
    assert(written == long(std::count(stored.begin(), stored.end(), true)));
    // 2. The load: the same grid, empty; the sidecar loads; AddTile runs per tile in grid order.
    FakeTerrain live(nx, ny); g_live = &live;
    auto* dec = new BlockCodec::DecodeScratch;
    assert(BeginApply(GridOf(live.cterrain), 0x5EED, path, dec) == written);
    assert(Loaded());
    long long c[7]{};
    for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) BigmapTestServeDetour(live.cterrain, int(1000 + i), FakeAddTile, Mark);
    BigmapTestServeCounters(c);
    assert(g_addCalls == nx * ny && c[0] == nx * ny);
    assert(c[1] == written && g_marked.size() == size_t(written));           // applied + marked per stored tile
    assert(c[3] == nx * ny - written && c[4] == 0 && c[2] == 0 && c[6] == 0);  // absent tiles untouched, every record found
    assert(c[5] == nx * ny);                                                  // in grid order the cursor hits first probe every time
    size_t m = 0;
    for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
        if (stored[i]) { assert(live.caches[i] == save.caches[i]); assert(g_marked[m++] == live.caches[i].data()); }
        else assert(std::all_of(live.caches[i].begin(), live.caches[i].end(), [](uint16_t h) { return h == 0; }));
    }
    // 3. Out-of-order AddTile still finds its record (more probes), and a
    //    failing mark (no tile arena) counts as unmarked but the decode happened.
    FakeTerrain live2(nx, ny); g_live = &live2; g_marked.clear();
    std::vector<uint32_t> order(size_t(nx * ny)); for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), std::mt19937(3));
    long long before[7]{}; BigmapTestServeCounters(before);
    for (uint32_t i : order) BigmapTestServeDetour(live2.cterrain, int(1000 + i), FakeAddTile, MarkFails);
    BigmapTestServeCounters(c);
    assert(c[2] - before[2] == written && c[1] == before[1] && c[4] == 0);
    assert(c[5] - before[5] > nx * ny);                                        // shuffled: the cursor misses
    for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) if (stored[i]) assert(live2.caches[i] == save.caches[i]);
    // 4. Not loaded (EndApply): AddTile runs, nothing is applied or probed.
    EndApply(); assert(!Loaded());
    FakeTerrain live3(nx, ny); g_live = &live3; BigmapTestServeCounters(before);
    BigmapTestServeDetour(live3.cterrain, 1000, FakeAddTile, Mark);
    BigmapTestServeCounters(c);
    assert(c[0] == before[0] + 1 && c[1] == before[1] && c[5] == before[5] && c[3] == before[3]);
    // 5. An entity AddTile never stored (stale sidecar index) is not found, not applied.
    assert(BeginApply(GridOf(live3.cterrain), 0x5EED, path, dec) == written);
    BigmapTestServeCounters(before);
    BigmapTestServeDetour(live3.cterrain, 999999, [](void*, int, uint64_t, uint64_t) {}, Mark);
    BigmapTestServeCounters(c);
    assert(c[4] == before[4] + 1 && c[1] == before[1]);
    EndApply();
    // 6. Armed by the LoadGame hook (fingerprint + path, BeginApply not yet run):
    //    the first AddTile opens and verifies the file against this grid, the
    //    rest serve; the pass-done callback releases it.
    TerrainSidecar::g_saveFingerprint = 0x5EED; strcpy_s(TerrainSidecar::g_sidecarPath, path); TerrainSidecar::g_pending = true;
    FakeTerrain live4(nx, ny); g_live = &live4; g_marked.clear(); BigmapTestServeCounters(before);
    for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
        BigmapTestServeDetour(live4.cterrain, int(1000 + i), FakeAddTile, Mark);
        assert(Loaded() && !TerrainSidecar::g_pending);
    }
    BigmapTestServeCounters(c);
    assert(c[1] - before[1] == written && c[4] == before[4]);
    for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) if (stored[i]) assert(live4.caches[i] == save.caches[i]);
    EndApply(); assert(!Loaded());
    // A foreign fingerprint armed: refused at the first AddTile, everything loads stock.
    TerrainSidecar::g_saveFingerprint = 0xBAD; TerrainSidecar::g_pending = true;
    FakeTerrain live5(nx, ny); g_live = &live5; BigmapTestServeCounters(before);
    BigmapTestServeDetour(live5.cterrain, 1000, FakeAddTile, Mark);
    assert(!Loaded() && !TerrainSidecar::g_pending);
    BigmapTestServeCounters(c); assert(c[1] == before[1] && c[5] == before[5]);
    remove(path);
    // 6. Byte anchors in the real executable.
    FILE* f = nullptr; fopen_s(&f, "C:\\tools\\bin\\TransportFever2.exe", "rb"); assert(f);
    auto check = [&](uintptr_t rva, const uint8_t* b, size_t n) {
        uint8_t buf[32]; fseek(f, long(rva - 0x1000 + 0x400), SEEK_SET); assert(fread(buf, 1, n, f) == n); assert(memcmp(buf, b, n) == 0);
    };
    check(TerrainServe::kAddTileRva, TerrainServe::kAddTileBytes, sizeof TerrainServe::kAddTileBytes);
    check(TerrainServe::kRecordStoreRva, TerrainServe::kRecordStoreBytes, sizeof TerrainServe::kRecordStoreBytes);
    check(TerrainServe::kDetachRva, TerrainServe::kDetachBytes, sizeof TerrainServe::kDetachBytes);
    fclose(f);
    printf("PASS: %ld of %d tiles served at AddTile in grid order (one probe each) and shuffled, absent/unloaded/unknown-entity cases, byte anchors\n", written, nx * ny);
    return 0;
}
