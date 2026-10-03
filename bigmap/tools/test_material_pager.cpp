// Real Windows mappings and access violations for the material-cell pager,
// isolated from the game. Optional argv[1]: live material cells sampled
// read-only (%TEMP%\tpf2-cache-visual\material-cells-1024.bin).
#include "../src/terrain_pager.h"
#include "../src/material_pager.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <cassert>

int main(int argc, char** argv) {
    using namespace MaterialPager;
    // Both pagers coexist: separate arenas, locks and exception handlers.
    assert(TerrainPager::Init(4 * TerrainPager::SlotBytes));
    assert(Init(4 * SlotBytes));
    assert(SlotBytes == 69632 && Bytes == 67601);
    std::vector<uint8_t> cell(Bytes);
    std::mt19937 rng(7);
    for (size_t y = 0; y < 260; ++y) for (size_t x = 0; x < 260; ++x)
        cell[y * 260 + x] = uint8_t(20 + ((x / 13 + y / 9) % 6) + ((rng() % 5) == 0));
    cell[Bytes - 1] = 'V';

    auto p = Allocate();
    assert(p && uintptr_t(p) % 32 == 0);
    for (size_t i = 0; i < Bytes; ++i) assert(p[i] == 0);
    memcpy(p, cell.data(), Bytes);
    auto t = TerrainPager::Allocate();
    assert(t && !Contains(t) && !TerrainPager::Contains(p));
    for (int n = 0; n < 50; ++n) {
        assert(Evict(Index(p), true));
        assert(memcmp(p, cell.data(), Bytes) == 0);                 // read fault restores
        size_t i = (n * 977) % Bytes; p[i] ^= 3; cell[i] ^= 3;         // write after RO restore
        assert(Evict(Index(p), true));
        i = (n * 4099) % Bytes; p[i] ^= 5; cell[i] ^= 5;               // write fault from cold
        assert(memcmp(p, cell.data(), Bytes) == 0);
        t[n] = uint16_t(n);                                            // other pager untouched
    }
    assert(Snapshot().failures == 0 && TerrainPager::Snapshot().failures == 0);
    assert(TerrainPager::Release(t));
    assert(Release(p) && !Release(p));

    // Noise cells (> 64 distinct bytes) are refused by the codec and stay resident.
    auto noise = Allocate();
    for (size_t i = 0; i < Bytes; ++i) noise[i] = uint8_t(rng());
    assert(!Evict(Index(noise), true));
    assert(Release(noise));

    // Concurrent engine-like access: pool workers copy a cell and its neighbours
    // (reads) and write their own cell back while the evictor races them.
    constexpr unsigned Cells = 32, Threads = 8, Iterations = 4000;
    std::vector<uint8_t*> cells(Cells);
    for (unsigned c = 0; c < Cells; ++c) { cells[c] = Allocate(); assert(cells[c]); memcpy(cells[c], cell.data(), Bytes); }
    std::atomic<bool> start{false}, done{false};
    std::thread evictor([&] { while (!start) SwitchToThread(); while (!done) for (auto c : cells) Evict(Index(c), true); });
    std::vector<std::thread> workers;
    std::vector<uint8_t> totals(Threads);
    for (unsigned w = 0; w < Threads; ++w) workers.emplace_back([&, w] {
        std::vector<uint8_t> cache(Bytes);
        while (!start) SwitchToThread();
        for (unsigned k = 0; k < Iterations; ++k) {
            unsigned own = w;                      // each worker owns one cell's byte 0
            unsigned neighbour = (w * 7 + k) % Cells;
            memcpy(cache.data(), cells[neighbour], Bytes);
            assert(cache[Bytes - 1] == 'V');
            uint8_t v = cells[own][0];
            cells[own][0] = uint8_t(v + 1);        // single writer per cell byte
            if (!(k % 97)) SwitchToThread();
        }
    });
    start = true;
    for (auto& w : workers) w.join();
    done = true;
    evictor.join();
    for (unsigned w = 0; w < Threads; ++w) assert(cells[w][0] == uint8_t(cell[0] + Iterations));
    for (auto c : cells) assert(Release(c));

    if (argc > 1) {
        FILE* f = nullptr;
        fopen_s(&f, argv[1], "rb");
        assert(f);
        size_t count = 0, compressed = 0;
        ULONGLONG t0 = GetTickCount64();
        while (fread(cell.data(), Bytes, 1, f) == 1) {
            p = Allocate(); assert(p);
            memcpy(p, cell.data(), Bytes);
            bool packed = Evict(Index(p), true);
            compressed += packed ? size_t(Snapshot().compressedBytes) : Bytes;
            assert(memcmp(p, cell.data(), Bytes) == 0);
            assert(Release(p));
            ++count;
        }
        fclose(f);
        printf("live cells=%zu ratio=%.4f roundtrip_ms=%llu\n", count, double(compressed) / (count * Bytes), GetTickCount64() - t0);
    }
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    std::vector<uint8_t*> many(2048);
    for (auto& c : many) { c = Allocate(); assert(c); memcpy(c, cell.data(), Bytes); }
    for (auto c : many) { Evict(Index(c), true); assert(memcmp(c, cell.data(), Bytes) == 0); Evict(Index(c), true); }
    for (auto c : many) assert(Release(c));
    GetProcessHandleCount(GetCurrentProcess(), &after);
    assert(before == after);
    auto s = Snapshot();
    assert(s.live == 0 && s.resident == 0 && s.compressedBytes == 0 && s.compressedCommit == 0 && s.failures == 0);
    printf("PASS: material pager exact roundtrips, write faults, noise fallback, coexisting terrain pager, "
           "%u workers racing eviction; faults=%llu evictions=%llu failures=%llu\n",
           Threads, s.faults, s.evictions, s.failures);
}
