// Round-trip, bounds and corruption tests for src/material_codec.h.
// Optional argv[1]: live material cells sampled read-only from a running game
// (%TEMP%\tpf2-cache-visual\material-cells-1024.bin, 67,601 bytes each).
#include "../src/material_codec.h"
#include <cstdio>
#include <vector>
#include <random>
#include <chrono>
#include <memory>

using namespace MaterialCodec;
using Clock = std::chrono::steady_clock;
static std::unique_ptr<EncodeScratch> enc(new EncodeScratch);
static std::unique_ptr<DecodeScratch> dec(new DecodeScratch);
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static size_t RoundTrip(const std::vector<uint8_t>& cell, size_t cap = Bytes) {
    std::vector<uint8_t> blob(cap + 16, 0xA5);
    size_t n = Encode(cell.data(), blob.data(), cap, *enc);
    for (size_t i = cap; i < blob.size(); ++i) CHECK(blob[i] == 0xA5);
    if (!n) return 0;
    std::vector<uint8_t> out(Bytes, 0x11);
    CHECK(Decode(blob.data(), n, out.data(), *dec));
    CHECK(out == cell);
    CHECK(!Decode(blob.data(), n - 1, out.data(), *dec));
    std::vector<uint8_t> longer(blob.begin(), blob.begin() + n);
    longer.push_back(0);
    CHECK(!Decode(longer.data(), longer.size(), out.data(), *dec));
    return n;
}

int main(int argc, char** argv) {
    std::mt19937 rng(20260915);
    std::vector<uint8_t> c(Bytes);
    for (uint8_t v : {uint8_t(0), uint8_t(86), uint8_t(255)}) { std::fill(c.begin(), c.end(), v); CHECK(RoundTrip(c) > 0); }
    // Alphabet sizes 2..64 round-trip; 65 distinct values are refused.
    for (unsigned d : {2u, 3u, 8u, 19u, 34u, 63u, 64u}) {
        for (auto& v : c) v = uint8_t(200 + rng() % d);
        c[Bytes - 1] = 'V';
        size_t n = RoundTrip(c);
        CHECK(n > 0 || d + 1 > MaxAlphabet);
    }
    for (size_t i = 0; i < Bytes; ++i) c[i] = uint8_t(i % 65);
    CHECK(Encode(c.data(), std::vector<uint8_t>(Bytes).data(), Bytes, *enc) == 0);
    // Dither-like cells: a few materials in blobs with noisy borders.
    for (int t = 0; t < 500; ++t) {
        unsigned d = 1 + rng() % 16;
        for (size_t y = 0; y < Width; ++y) for (size_t x = 0; x < Width; ++x) {
            unsigned base = unsigned((x / (8 + t % 40) + y / (5 + t % 30)) % d);
            c[y * Width + x] = uint8_t(20 + ((rng() % 7) ? base : rng() % d));
        }
        c[Bytes - 1] = 'V';
        CHECK(RoundTrip(c) > 0);
    }
    // Corruption never crashes and is never silently accepted.
    std::vector<uint8_t> blob(Bytes);
    size_t n = Encode(c.data(), blob.data(), blob.size(), *enc);
    CHECK(n > 0);
    std::vector<uint8_t> out(Bytes);
    int silent = 0;
    for (int k = 0; k < 5000; ++k) {
        std::vector<uint8_t> bad(blob.begin(), blob.begin() + n);
        bad[rng() % n] ^= uint8_t(1 + rng() % 255);
        if (Decode(bad.data(), bad.size(), out.data(), *dec) && out != c) ++silent;
    }
    CHECK(silent == 0);
    printf("corruption: 5000 single-byte flips, %d silently accepted\n", silent);

    if (argc > 1) {
        FILE* f = nullptr;
        fopen_s(&f, argv[1], "rb");
        CHECK(f != nullptr);
        std::vector<std::vector<uint8_t>> cells;
        while (f && fread(c.data(), Bytes, 1, f) == 1) cells.push_back(c);
        if (f) fclose(f);
        size_t total = 0, refused = 0;
        double es = 0, ds = 0;
        std::vector<uint8_t> b(Bytes);
        for (auto& cell : cells) {
            auto t0 = Clock::now();
            size_t m = Encode(cell.data(), b.data(), Bytes * 95 / 100, *enc);
            auto t1 = Clock::now();
            if (!m) { ++refused; total += Bytes; continue; }
            CHECK(Decode(b.data(), m, out.data(), *dec));
            auto t2 = Clock::now();
            CHECK(out == cell);
            total += m;
            es += std::chrono::duration<double>(t1 - t0).count();
            ds += std::chrono::duration<double>(t2 - t1).count();
        }
        if (!cells.empty())
            printf("live cells=%zu refused=%zu ratio=%.4f mean_bytes=%.0f encode_us=%.1f decode_us=%.1f "
                   "projected_gib: 25992 tiles %.3f, 65536 tiles %.3f, 262144 tiles %.3f\n",
                   cells.size(), refused, double(total) / (cells.size() * Bytes), double(total) / cells.size(),
                   es * 1e6 / cells.size(), ds * 1e6 / cells.size(),
                   double(total) / cells.size() * 25992 / (1 << 30), double(total) / cells.size() * 65536 / (1 << 30),
                   double(total) / cells.size() * 262144 / (1 << 30));
    }
    printf(failures ? "FAILED: %d\n" : "PASS: material codec round trips, alphabet limit, bounds and corruption\n", failures);
    return failures ? 1 : 0;
}
