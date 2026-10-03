// Stage timing for TerrainCodec::Decode on real tiles: header/table setup,
// sample loop, content hash. Offline only.
#include "../src/terrain_codec.h"
#include <windows.h>
#include <vector>
#include <cstdio>
#include <memory>

using namespace TerrainCodec;
static double Now() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return double(t.QuadPart) / f.QuadPart; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = nullptr; fopen_s(&f, argv[1], "rb"); if (!f) return 2;
    std::vector<std::vector<uint16_t>> tiles; std::vector<uint16_t> t(Samples), out(Samples);
    while (fread(t.data(), RawBytes, 1, f) == 1) tiles.push_back(t);
    fclose(f);
    std::unique_ptr<EncodeScratch> es(new EncodeScratch);
    std::unique_ptr<DecodeScratch> ds(new DecodeScratch);
    std::vector<std::vector<uint8_t>> blobs;
    for (auto& tile : tiles) {
        std::vector<uint8_t> b(RawBytes);
        size_t n = Encode(tile.data(), b.data(), b.size(), *es);
        b.resize(n); blobs.push_back(b);
    }
    const int reps = 3;
    double header = 0, full = 0, noHash = 0, hash = 0;
    for (int r = 0; r < reps; ++r) {
        for (auto& b : blobs) {
            uint32_t rans = uint32_t(b[13]) | uint32_t(b[14]) << 8 | uint32_t(b[15]) << 16 | uint32_t(b[16]) << 24;
            double a = Now(); Decode(b.data(), b.size() - rans, out.data(), *ds); header += Now() - a;
            a = Now(); bool ok1 = Decode(b.data(), b.size(), out.data(), *ds, false); noHash += Now() - a;
            a = Now(); volatile uint64_t h = Hash(out.data()); hash += Now() - a; (void)h;
            a = Now(); bool ok2 = Decode(b.data(), b.size(), out.data(), *ds, true); full += Now() - a;
            if (!ok1 || !ok2) { printf("decode failed\n"); return 1; }
        }
    }
    double k = 1e6 / (reps * blobs.size());
    printf("tiles=%zu header_tables_us=%.1f loop_plus_header_us=%.1f hash_us=%.1f full_us=%.1f\n",
           blobs.size(), header * k, noHash * k, hash * k, full * k);
}
