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
#include <atomic>
#include <thread>
#include <mutex>
#include "../../native/src/plugin/tpf2mp_plugin.h"
static const Tpf2mpHost* H = nullptr;
static bool g_gog = false;
// The plugin couples the hook to the tile arena; the test supplies its own mark.
namespace TerrainPager { static bool SetServed(const void*) { return false; } static bool IsServed(const void*) { return false; } }
static bool (*g_terrainServedCheck)(const void*) = nullptr;
static volatile LONG64 g_terrainServedCopiesSkipped = 0;
static void (*g_alignmentPassDone)(bool) = nullptr;
static bool (*g_alignmentAllServed)(void*) = nullptr;
#include "../src/terrain_sidecar.h"
#include "../src/terrain_serve.h"
using namespace TerrainSidecar;

struct FakeTerrain {
    uint8_t cterrain[0x40];   // +0x18 the grid, +0x34 the height scale
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
    assert(c[5] == nx * ny);                                                  // in grid order the search hits on the first probe every time
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
    // 3b. Two AddTile threads at once, each in grid order in its own half (the
    //     load's shape): per-thread cursors keep it near one probe per tile. One
    //     shared cursor started each scan where the other thread was: ~n/2 probes
    //     per tile (2026-09-28: 6.6 billion probes for 221,952 tiles).
    {
        FakeTerrain live4(nx, ny); g_live = &live4;
        long long b4[7]{}; BigmapTestServeCounters(b4);
        const uint32_t half = uint32_t(nx * ny) / 2;
        std::atomic<int> turn{0};
        auto run = [&](int me) {
            for (uint32_t k = 0; k < half; ++k) {
                while (turn.load() != me) {}
                BigmapTestServeDetour(live4.cterrain, int(1000 + me * half + k), FakeAddTile, MarkFails);
                turn.store(1 - me);
            }
        };
        std::thread t0(run, 0), t1(run, 1); t0.join(); t1.join();
        BigmapTestServeCounters(c);
        const long long probed = c[5] - b4[5];
        printf("two AddTile threads: %lld probes for %d tiles\n", probed, nx * ny);
        assert(probed <= 2LL * nx * ny);
        for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) if (stored[i]) assert(live4.caches[i] == save.caches[i]);
    }
    // 3c. DESCENDING order (a forward scan wrapped almost the whole grid per tile:
    //     ~35,000 probes each on a 36,992-tile load): two probes per tile.
    {
        FakeTerrain live5(nx, ny); g_live = &live5;
        long long b5[7]{}; BigmapTestServeCounters(b5);
        std::thread([&] {                                                   // a fresh thread: its own cursor
            for (int i = nx * ny - 1; i >= 0; --i) BigmapTestServeDetour(live5.cterrain, 1000 + i, FakeAddTile, MarkFails);
        }).join();
        BigmapTestServeCounters(c);
        printf("descending AddTile: %lld probes for %d tiles\n", c[5] - b5[5], nx * ny);
        assert(c[5] - b5[5] <= 3LL * nx * ny && c[4] == b5[4]);
        for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) if (stored[i]) assert(live5.caches[i] == save.caches[i]);
    }
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
    // 7. Threads (a player's crash dump, 2026-09-26): the game runs the alignment
    //    pass on many threads and each finishing pass releases the file, while
    //    AddTile threads may still decode from it. Exactly one release wins, and
    //    a decode either completes intact or finds nothing loaded.
    {
        std::vector<FakeTerrain*> readers;
        for (int t = 0; t < 4; ++t) {
            auto* ft = new FakeTerrain(nx, ny);
            for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) ft->addTile(i, int(1000 + i));
            readers.push_back(ft);
        }
        for (int round = 0; round < 200; ++round) {
            assert(BeginApply(GridOf(live3.cterrain), 0x5EED, path, dec) == written);
            volatile LONG go = 0, released = 0, intact = 0;
            std::vector<HANDLE> threads;
            struct Reader { FakeTerrain* t; volatile LONG* go; volatile LONG* intact; const FakeTerrain* save; };
            struct Ender { volatile LONG* go; volatile LONG* released; };
            std::vector<Reader> rs; std::vector<Ender> es;
            rs.reserve(readers.size()); es.reserve(8);
            for (auto* ft : readers) rs.push_back({ft, &go, &intact, &save});
            for (int e = 0; e < 8; ++e) es.push_back({&go, &released});
            for (auto& r : rs) threads.push_back(CreateThread(nullptr, 0, [](void* p) -> DWORD {
                auto* r = static_cast<Reader*>(p);
                auto* scratch = new BlockCodec::DecodeScratch;
                while (!*r->go) YieldProcessor();
                for (uint32_t i = 0; i < uint32_t(r->t->caches.size()); ++i)
                    if (ApplyTile(GridOf(r->t->cterrain), i, *scratch)) {
                        assert(r->t->caches[i] == r->save->caches[i]);
                        InterlockedIncrement(r->intact);
                    }
                delete scratch;
                return 0;
            }, &r, 0, nullptr));
            for (auto& e : es) threads.push_back(CreateThread(nullptr, 0, [](void* p) -> DWORD {
                auto* e = static_cast<Ender*>(p);
                while (!*e->go) YieldProcessor();
                if (EndApply()) InterlockedIncrement(e->released);
                return 0;
            }, &e, 0, nullptr));
            InterlockedExchange(&go, 1);
            WaitForMultipleObjects(DWORD(threads.size()), threads.data(), TRUE, INFINITE);
            for (HANDLE h : threads) CloseHandle(h);
            assert(released == 1 && !Loaded());
            assert(!EndApply());
        }
        for (auto* ft : readers) delete ft;
    }
    // 8. A skipped alignment pass (AllServedFinish): only when every record of
    //    the terrain is served and the sidecar is loaded; then each record gets
    //    publication's minZ/maxZ (float(v) * scale) and one more version.
    {
        static std::vector<const void*> served;
        g_terrainServedCheck = [](const void* p) { return std::find(served.begin(), served.end(), p) != served.end(); };
        const char* full = "test_serve_full.bin";
        FakeTerrain all(nx, ny);
        for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
            all.fill(i, i * 5 + 3);
            *reinterpret_cast<int32_t*>(all.record(i) + 0) = int32_t(1000 + i);
            *reinterpret_cast<uint8_t**>(all.record(i) + 8) = all.controls[i].data() + 0x10;
            *reinterpret_cast<uint8_t**>(all.record(i) + 0x10) = all.controls[i].data();
            auto* v = all.vec(i); v->first = all.caches[i].data(); v->last = v->end = v->first + Samples;
        }
        uint64_t fb = 0;
        assert(Write(GridOf(all.cterrain), 0x5EED, full, enc, &fb) == nx * ny);
        auto load = [&](FakeTerrain& t, const char* file, long want) {
            TerrainSidecar::g_saveFingerprint = 0x5EED; strcpy_s(TerrainSidecar::g_sidecarPath, file); TerrainSidecar::g_pending = true;
            g_live = &t; served.clear();
            *reinterpret_cast<float*>(t.cterrain + 0x34) = 0.25f;
            for (uint32_t i = 0; i < uint32_t(nx * ny); ++i)
                BigmapTestServeDetour(t.cterrain, int(1000 + i), FakeAddTile, [](const void* p) { served.push_back(p); return true; });
            assert(Loaded() == (want > 0) && long(served.size()) == want);
        };
        FakeTerrain fullLoad(nx, ny);
        load(fullLoad, full, nx * ny);
        // the load's second CTerrain version: its own grid, served from the same file
        FakeTerrain second(nx, ny); g_live = &second;
        *reinterpret_cast<float*>(second.cterrain + 0x34) = 0.25f;
        for (uint32_t i = 0; i < uint32_t(nx * ny); ++i)
            BigmapTestServeDetour(second.cterrain, int(1000 + i), FakeAddTile, [](const void* p) { served.push_back(p); return true; });
        assert(BigmapTestServeVersionPending() == 1);
        assert(BigmapTestServeAllServedFinish(second.cterrain) == 1);
        assert(BigmapTestServeVersionPending() == 1);                      // the first version has not passed yet: keep the file
        assert(BigmapTestServeAllServedFinish(second.cterrain) == 0);      // once per version: a later pass on it runs
        assert(BigmapTestServeAllServedFinish(fullLoad.cterrain) == 1);   // the first version's ranges survived the second's
        assert(BigmapTestServeVersionPending() == 0);
        for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
            const auto& c = all.caches[i];
            const uint16_t lo = *std::min_element(c.begin(), c.end()), hi = *std::max_element(c.begin(), c.end());
            assert(*reinterpret_cast<float*>(fullLoad.record(i) + 0x18) == float(lo) * 0.25f);
            assert(*reinterpret_cast<float*>(fullLoad.record(i) + 0x1c) == float(hi) * 0.25f);
            assert(*reinterpret_cast<int32_t*>(fullLoad.record(i) + 0x20) == 2);   // AddTile's + publication's
        }
        EndApply();
        assert(BigmapTestServeAllServedFinish(fullLoad.cterrain) == 0);          // released: a pass in play always runs
        // 70% of the tiles in the sidecar: not all served, nothing written
        FakeTerrain partLoad(nx, ny);
        load(partLoad, path, written);
        assert(BigmapTestServeAllServedFinish(partLoad.cterrain) == 0);
        for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
            assert(*reinterpret_cast<int32_t*>(partLoad.record(i) + 0x20) == 1);
            assert(*reinterpret_cast<float*>(partLoad.record(i) + 0x18) == 0.0f);
        }
        EndApply();
        // every tile applied, but one is no longer served (the arena let it go): refused
        FakeTerrain oneGone(nx, ny);
        load(oneGone, full, nx * ny);
        served.pop_back();
        assert(BigmapTestServeAllServedFinish(oneGone.cterrain) == 0);
        EndApply();
        // THE STREAM (terrain-stream.md): no sidecar beside the save, the host's
        // arriving in <data>/terrain_stream/ only after every tile was added.
        // The pass finds it, serves every tile there and is skipped.
        {
            CreateDirectoryA("test_stream_data", nullptr); CreateDirectoryA("test_stream_data/terrain_stream", nullptr);
            TerrainSidecar::SetStreamDir("test_stream_data");
            FakeTerrain late(nx, ny);
            load(late, "test_serve_absent.bin", 0);                       // nothing beside the save: nothing loaded
            assert(!Loaded() && TerrainSidecar::g_streamWanted);
            assert(CopyFileA(full, "test_stream_data/terrain_stream/77.terr", FALSE));
            TerrainServe::mark = [](const void* p) {          // CatchUp marks from several threads
                static std::mutex m; std::lock_guard<std::mutex> l(m); served.push_back(p); return true; };
            assert(BigmapTestServeAllServedFinish(late.cterrain) == 1);
            for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
                assert(memcmp(late.caches[i].data(), all.caches[i].data(), Samples * 2) == 0);
                assert(*reinterpret_cast<int32_t*>(late.record(i) + 0x20) == 2);
            }
            assert(BigmapTestServeAllServedFinish(late.cterrain) == 0);   // passed once: a pass in play runs
            EndApply();
            TerrainSidecar::SetStreamDir(nullptr);
            assert(GetFileAttributesA("test_stream_data/terrain_stream/77.terr.done") != INVALID_FILE_ATTRIBUTES);   // the lobby stops the rest
            DeleteFileA("test_stream_data/terrain_stream/77.terr"); DeleteFileA("test_stream_data/terrain_stream/77.terr.done");
            RemoveDirectoryA("test_stream_data/terrain_stream"); RemoveDirectoryA("test_stream_data");
        }
        // DECODE AT THE PASS (terrain_sidecar_decode_at_pass): AddTile serves
        // nothing; each version's pass decodes all its tiles in parallel and is
        // skipped, and the file stays until the second version has passed.
        {
            BigmapTestServeAtPass(1);
            TerrainServe::ForgetTerrains();
            FakeTerrain v1(nx, ny), v2(nx, ny);
            *reinterpret_cast<float*>(v1.cterrain + 0x34) = 0.25f; *reinterpret_cast<float*>(v2.cterrain + 0x34) = 0.25f;
            TerrainSidecar::g_saveFingerprint = 0x5EED; strcpy_s(TerrainSidecar::g_sidecarPath, full); TerrainSidecar::g_pending = true;
            served.clear();
            static std::mutex sm;
            auto markSafe = [](const void* p) { std::lock_guard<std::mutex> l(sm); served.push_back(p); return true; };
            g_live = &v1; for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) BigmapTestServeDetour(v1.cterrain, int(1000 + i), FakeAddTile, markSafe);
            g_live = &v2; for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) BigmapTestServeDetour(v2.cterrain, int(1000 + i), FakeAddTile, markSafe);
            assert(Loaded() && served.empty());                                   // nothing decoded at AddTile
            for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) assert(v1.caches[i] != all.caches[i]);
            TerrainServe::mark = markSafe;
            assert(BigmapTestServeAllServedFinish(v1.cterrain) == 1);
            assert(BigmapTestServeVersionPending() == 1 && Loaded());            // v2 seen at AddTile, not passed: keep the file
            assert(BigmapTestServeAllServedFinish(v2.cterrain) == 1);
            assert(BigmapTestServeVersionPending() == 0);
            for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
                assert(v1.caches[i] == all.caches[i] && v2.caches[i] == all.caches[i]);
                assert(*reinterpret_cast<int32_t*>(v1.record(i) + 0x20) == 2 && *reinterpret_cast<int32_t*>(v2.record(i) + 0x20) == 2);
            }
            assert(served.size() == size_t(2 * nx * ny));
            EndApply();
            TerrainServe::ForgetTerrains();
            BigmapTestServeAtPass(0);
        }
        remove(full);
        g_terrainServedCheck = nullptr;
    }
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
    printf("PASS: %ld of %d tiles served at AddTile in grid order (one probe each) and shuffled, absent/unloaded/unknown-entity cases, one release under threads, byte anchors\n", written, nx * ny);
    return 0;
}
