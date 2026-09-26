// The terrain sidecar format and grid walk against a synthetic terrain grid
// built to the game's exact layout (CTerrain+0x18 -> {x0,y0,nx,ny,records};
// 40-byte records; record+8 = the vector object, record+0x10 its control block). No game.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <random>
#include <cassert>
#include <array>
#include <string>
#include "../src/terrain_sidecar.h"

using namespace TerrainSidecar;

// A stand-in CTerrain: a pointer at +0x18 to a grid blob, which holds the
// record array inline after its 0x18-byte head.
struct FakeTerrain {
    uint8_t cterrain[0x20];
    std::vector<uint8_t> grid;
    std::vector<std::vector<uint16_t>> caches;               // backing for each vector's data
    std::vector<std::array<uint8_t, 0x20>> controls;         // the shared control block per record
    FakeTerrain(int nx, int ny) {
        memset(cterrain, 0, sizeof cterrain);
        grid.assign(0x18 + size_t(nx) * ny * 40, 0);
        *reinterpret_cast<int32_t*>(grid.data() + 0) = 0;
        *reinterpret_cast<int32_t*>(grid.data() + 4) = 0;
        *reinterpret_cast<int32_t*>(grid.data() + 8) = nx;
        *reinterpret_cast<int32_t*>(grid.data() + 0xc) = ny;
        caches.resize(size_t(nx) * ny);
        controls.assign(size_t(nx) * ny, {});               // fixed addresses for control blocks
    }
    void finalize() {
        *reinterpret_cast<uint8_t**>(grid.data() + 0x10) = grid.data() + 0x18;
        *reinterpret_cast<uint8_t**>(cterrain + 0x18) = grid.data();
    }
    uint8_t* record(uint32_t i) { return grid.data() + 0x18 + size_t(i) * 40; }
    // Give record i a height cache of `n` samples filled from `seed`, laid out as
    // make_shared does: block+0 control, block+0x10 the vector {first,last,end};
    // record+8 = the vector object, record+0x10 = the control block.
    void makeTile(uint32_t i, size_t n, uint32_t seed) {
        auto& c = caches[i]; c.resize(n);
        std::mt19937 rng(seed); uint16_t h = uint16_t(20000 + rng() % 500);
        for (size_t k = 0; k < n; ++k) { h = uint16_t(h + int(rng() % 7) - 3); c[k] = h; }
        *reinterpret_cast<uint8_t**>(record(i) + 8) = controls[i].data() + 0x10;   // the vector object
        *reinterpret_cast<uint8_t**>(record(i) + 0x10) = controls[i].data();       // its control block
        auto* v = reinterpret_cast<TileVector*>(controls[i].data() + 0x10);
        v->first = c.data(); v->last = c.data() + n; v->end = c.data() + n;
        *reinterpret_cast<int32_t*>(record(i) + 0) = int32_t(i);       // entity
        *reinterpret_cast<int32_t*>(record(i) + 0x20) = 1;             // version
    }
};

int main() {
    const int nx = 40, ny = 30;   // 1,200 records
    const char* path = "test_sidecar.bin";
    FakeTerrain save(nx, ny);
    std::vector<uint32_t> live;
    std::mt19937 pick(1);
    for (uint32_t i = 0; i < uint32_t(nx * ny); ++i) {
        // ~70% are full 1 m tiles; a few are wrong-sized (holes) to be skipped.
        if (pick() % 10 < 7) { save.makeTile(i, Samples, i * 7 + 1); live.push_back(i); }
        else if (pick() % 3 == 0) save.makeTile(i, 129 * 129, i);   // half-res: not eligible
    }
    save.finalize();
    auto* enc = new BlockCodec::EncodeScratch;
    uint64_t bytes = 0;
    long written = Write(GridOf(save.cterrain), 0xABCDEF12u, path, enc, &bytes);
    assert(written == long(live.size()));
    printf("sidecar: %ld tiles, %.2f MiB compressed, %.1f%% of raw\n",
           written, bytes / 1048576.0, 100.0 * bytes / (double(live.size()) * Samples * 2));

    // Apply into a fresh grid with the SAME tiles present but zeroed caches.
    FakeTerrain load(nx, ny);
    for (uint32_t i : live) { load.makeTile(i, Samples, 0); for (auto& x : load.caches[i]) x = 0; }
    // An eligible tile the save did NOT store must be left alone.
    std::vector<char> stored(nx * ny, 0); for (uint32_t i : live) stored[i] = 1;
    uint32_t unused = 0; while (unused < uint32_t(nx * ny) && stored[unused]) ++unused;
    load.makeTile(unused, Samples, 12345); auto probe = load.caches[unused];
    load.finalize();
    auto* dec = new BlockCodec::DecodeScratch;
    long applied = Apply(GridOf(load.cterrain), 0xABCDEF12u, path, dec);
    assert(applied == long(live.size()));
    for (uint32_t i : live) assert(load.caches[i] == save.caches[i]);   // exact restore
    assert(load.caches[unused] == probe);                               // untouched
    printf("apply: %ld tiles restored exactly\n", applied);

    // Fingerprint mismatch, dimension mismatch, and absent file are all no-ops.
    assert(Apply(GridOf(load.cterrain), 0xDEADBEEFu, path, dec) == 0);
    FakeTerrain small(nx, ny - 1); small.finalize();
    assert(Apply(GridOf(small.cterrain), 0xABCDEF12u, path, dec) == 0);
    assert(Apply(GridOf(load.cterrain), 0xABCDEF12u, "does_not_exist.bin", dec) == 0);

    // A truncated file is reported corrupt (-1), never a wrong restore.
    { FILE* f = nullptr; fopen_s(&f, path, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
      std::vector<uint8_t> buf(sz); fopen_s(&f, path, "rb"); assert(fread(buf.data(), 1, sz, f) == size_t(sz)); fclose(f);
      const char* tp = "test_sidecar_trunc.bin"; fopen_s(&f, tp, "wb"); fwrite(buf.data(), 1, sz - 100, f); fclose(f);
      assert(Apply(GridOf(load.cterrain), 0xABCDEF12u, tp, dec) == -1); remove(tp); }

    // A flipped byte in the compressed body is caught by the codec hash.
    { FILE* f = nullptr; fopen_s(&f, path, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
      std::vector<uint8_t> buf(sz); fopen_s(&f, path, "rb"); assert(fread(buf.data(), 1, sz, f) == size_t(sz)); fclose(f);
      buf[sizeof(FileHeader) + sizeof(TileHeader) + 20] ^= 0x40;
      const char* cp = "test_sidecar_corrupt.bin"; fopen_s(&f, cp, "wb"); fwrite(buf.data(), 1, sz, f); fclose(f);
      long r = Apply(GridOf(load.cterrain), 0xABCDEF12u, cp, dec); assert(r == -1); remove(cp); }

    // ---- Streaming per-tile load-side API (what the pass consumes). ----
    // Reuse the same grid; zero the live caches, then apply tile by tile.
    FakeTerrain stream(nx, ny);
    for (uint32_t i : live) { stream.makeTile(i, Samples, 0); for (auto& x : stream.caches[i]) x = 0; }
    stream.finalize();
    long ready = BeginApply(GridOf(stream.cterrain), 0xABCDEF12u, path, dec);
    assert(ready == long(live.size()) && Loaded());
    // Has() is true for exactly the stored tiles.
    for (uint32_t i : live) assert(Has(i));
    assert(!Has(unused) && !Has(uint32_t(nx * ny) + 5));
    // ApplyTile fills each stored tile exactly; a non-stored eligible tile is refused.
    auto* dec2 = new BlockCodec::DecodeScratch;
    for (uint32_t i : live) assert(ApplyTile(GridOf(stream.cterrain), i, *dec2));
    for (uint32_t i : live) assert(stream.caches[i] == save.caches[i]);
    { FakeTerrain solo(nx, ny); solo.makeTile(unused, Samples, 7); solo.finalize();
      assert(!ApplyTile(GridOf(solo.cterrain), unused, *dec2)); }   // not in the sidecar
    EndApply();
    assert(!Loaded() && !Has(live[0]));
    // BeginApply refuses a foreign fingerprint (holds nothing).
    assert(BeginApply(GridOf(stream.cterrain), 0xDEADBEEFu, path, dec) == 0 && !Loaded());

    // BeginApply no longer decodes up front: a file with ONE corrupt tile blob
    // still loads, and the bad tile surfaces as a false from ApplyTile (so the
    // caller falls back to compute for it) while every other tile restores.
    {
        FILE* f = nullptr; fopen_s(&f, path, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
        std::vector<uint8_t> buf(sz); fopen_s(&f, path, "rb"); assert(fread(buf.data(), 1, sz, f) == size_t(sz)); fclose(f);
        TileHeader first{}; memcpy(&first, buf.data() + sizeof(FileHeader), sizeof first);
        buf[sizeof(FileHeader) + sizeof(TileHeader) + 30] ^= 0x40;   // flip a byte in the first tile's body
        const char* cp = "test_sidecar_1bad.bin"; fopen_s(&f, cp, "wb"); fwrite(buf.data(), 1, sz, f); fclose(f);
        for (uint32_t i : live) { for (auto& x : stream.caches[i]) x = 0; }
        assert(BeginApply(GridOf(stream.cterrain), 0xABCDEF12u, cp, dec) == long(live.size()) && Has(first.index));
        int failures = 0;
        for (uint32_t i : live) if (!ApplyTile(GridOf(stream.cterrain), i, *dec2)) ++failures;
        assert(failures == 1 && !ApplyTile(GridOf(stream.cterrain), first.index, *dec2));   // exactly the corrupt tile
        for (uint32_t i : live) if (i != first.index) assert(stream.caches[i] == save.caches[i]);
        EndApply(); remove(cp);
    }
    delete dec2;

    // ---- RecordIndex / IndexOfRecord on a windowed grid (x0,y0 != 0). ----
    {
        FakeTerrain w(nx, ny);
        *reinterpret_cast<int32_t*>(w.grid.data() + 0) = 100;   // x0
        *reinterpret_cast<int32_t*>(w.grid.data() + 4) = -50;   // y0
        w.finalize();
        Grid g = GridOf(w.cterrain);
        assert(RecordIndex(g, 100, -50) == 0);                          // origin -> index 0
        assert(RecordIndex(g, 101, -50) == 1);                          // +x
        assert(RecordIndex(g, 100, -49) == nx);                         // +y -> +nx
        assert(RecordIndex(g, 100 + nx - 1, -50 + ny - 1) == long(nx) * ny - 1);
        assert(RecordIndex(g, 99, -50) == -1 && RecordIndex(g, 100, -51) == -1);   // outside window
        assert(RecordIndex(g, 100 + nx, -50) == -1);
        // IndexOfRecord agrees, and matches GetTile's formula for every cell.
        for (long i = 0; i < long(nx) * ny; i += 137) assert(IndexOfRecord(g, g.record(uint32_t(i))) == i);
        assert(IndexOfRecord(g, g.records() + 40 * nx * ny) == -1);     // one past the end
        assert(IndexOfRecord(g, g.record(0) + 8) == -1);               // misaligned
        printf("index: RecordIndex and IndexOfRecord agree with the engine's (x-x0)+(y-y0)*nx\n");
    }

    // ---- Fingerprint + arm/begin/end + WriteForSave glue. ----
    {
        // A stand-in .sav file; its hash is the fingerprint.
        const char* sav = "test_glue.sav";
        { FILE* f = nullptr; fopen_s(&f, sav, "wb"); std::mt19937 r(9); std::vector<uint8_t> b(300000); for (auto& x : b) x = uint8_t(r()); fwrite(b.data(), 1, b.size(), f); fclose(f); }
        char terr[520]; SidecarPath(sav, terr, sizeof terr);
        assert(std::string(terr) == "test_glue.terr");
        uint64_t fp1 = HashFile(sav), fp2 = HashFile(sav);
        assert(fp1 && fp1 == fp2);                                  // deterministic, non-zero
        assert(HashFile("no_such.sav") == 0);
        // WriteForSave hashes the .sav and writes <base>.terr beside it.
        FakeTerrain src(nx, ny);
        for (uint32_t i : live) src.makeTile(i, Samples, i * 7 + 1);   // same data as `save`
        src.finalize();
        long w2 = WriteForSave(src.cterrain, sav, enc, nullptr);
        assert(w2 == long(live.size()));
        // Arm from the same .sav, then BeginIfPending with the CTerrain.
        FakeTerrain dstg(nx, ny);
        for (uint32_t i : live) { dstg.makeTile(i, Samples, 0); for (auto& x : dstg.caches[i]) x = 0; }
        dstg.finalize();
        ArmForLoad(sav);
        assert(g_pending && g_saveFingerprint == fp1);
        assert(BeginIfPending(dstg.cterrain, dec) && Loaded() && !g_pending);
        assert(BeginIfPending(dstg.cterrain, dec) && Loaded());     // idempotent once loaded
        for (uint32_t i : live) assert(ApplyTile(GridOf(dstg.cterrain), i, *dec));
        for (uint32_t i : live) assert(dstg.caches[i] == src.caches[i]);
        EndApply();
        // A tampered .sav (different bytes) arms a different fingerprint, so the
        // stale .terr is rejected: BeginIfPending loads nothing.
        { FILE* f = nullptr; fopen_s(&f, sav, "ab"); uint8_t z = 0; fwrite(&z, 1, 1, f); fclose(f); }
        ArmForLoad(sav);
        assert(g_saveFingerprint != fp1);
        assert(!BeginIfPending(dstg.cterrain, dec) && !Loaded());
        // Not armed -> BeginIfPending is a no-op (per-frame passes during play).
        g_pending = false; assert(!BeginIfPending(dstg.cterrain, dec) && !Loaded());
        remove(sav); remove(terr);
    }

    remove(path); delete enc; delete dec;
    printf("PASS: write walks the grid, apply restores exactly, foreign/stale/absent are no-ops, truncation and corruption are rejected; streaming BeginApply/Has/ApplyTile restore per tile and reject a foreign fingerprint; fingerprint+arm/begin/end and WriteForSave round-trip, a changed .sav rejects the stale sidecar\n");
    return 0;
}
