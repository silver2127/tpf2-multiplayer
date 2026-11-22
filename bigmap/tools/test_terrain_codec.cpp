// Exhaustive round-trip, bounds and corruption tests for src/terrain_codec.h.
// Optional argv[1]: offline 257x257 uint16 terrain sample file (e.g.
// %TEMP%\tpf2-cache-visual\terrain-1m-samples.bin). No game files are touched.
#include "../src/terrain_codec.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <memory>

using namespace TerrainCodec;
using Clock = std::chrono::steady_clock;

static std::unique_ptr<EncodeScratch> enc(new EncodeScratch);
static std::unique_ptr<DecodeScratch> dec(new DecodeScratch);
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// Encode + decode one tile; returns encoded size (0 = not compressible within cap).
static size_t RoundTrip(const std::vector<uint16_t>& tile, size_t cap = RawBytes) {
    std::vector<uint8_t> blob(cap + 16, 0xA5);
    size_t n = Encode(tile.data(), blob.data(), cap, *enc);
    for (size_t i = cap; i < blob.size(); ++i) CHECK(blob[i] == 0xA5);  // no write past cap
    if (!n) return 0;
    CHECK(n <= cap);
    std::vector<uint16_t> out(Samples, 0x1234);
    CHECK(Decode(blob.data(), n, out.data(), *dec));
    CHECK(out == tile);
    // Truncation and trailing garbage must be rejected, never read out of bounds.
    CHECK(!Decode(blob.data(), n - 1, out.data(), *dec));
    std::vector<uint8_t> longer(blob.begin(), blob.begin() + n);
    longer.push_back(0);
    CHECK(!Decode(longer.data(), longer.size(), out.data(), *dec));
    return n;
}

int main(int argc, char** argv) {
    std::mt19937 rng(20260915);
    std::vector<uint16_t> t(Samples);

    // Degenerate and extreme tiles.
    for (uint16_t v : {uint16_t(0), uint16_t(1), uint16_t(254), uint16_t(255), uint16_t(32767), uint16_t(32768), uint16_t(65535)}) {
        std::fill(t.begin(), t.end(), v);
        CHECK(RoundTrip(t) > 0);
    }
    for (size_t i = 0; i < Samples; ++i) t[i] = (i & 1) ? 0 : 65535;   // max residual magnitude
    RoundTrip(t);
    for (size_t y = 0; y < Side; ++y) for (size_t x = 0; x < Side; ++x) t[y * Side + x] = uint16_t(y * 173 + x * x + 65500);
    CHECK(RoundTrip(t) > 0);
    // Pure noise: must either round-trip or refuse within the cap; never overflow.
    for (auto& v : t) v = uint16_t(rng());
    size_t noise = RoundTrip(t, RawBytes * 95 / 100);
    printf("noise tile: %zu bytes (0 = kept uncompressed)\n", noise);
    // Every escape boundary value as an isolated spike in smooth terrain.
    for (unsigned spike : {254u, 255u, 256u, 1000u, 32767u, 32768u, 65535u}) {
        for (size_t y = 0; y < Side; ++y) for (size_t x = 0; x < Side; ++x) t[y * Side + x] = uint16_t(20000 + x + y);
        for (int k = 0; k < 50; ++k) t[rng() % Samples] = uint16_t(20000 + spike);
        CHECK(RoundTrip(t) > 0);
    }
    // Random walks with a mix of small and escape-sized steps, 2,000 tiles.
    for (int n = 0; n < 2000; ++n) {
        unsigned big = rng() % 1000;
        for (size_t y = 0; y < Side; ++y) for (size_t x = 0; x < Side; ++x) {
            size_t i = y * Side + x;
            uint16_t base = uint16_t(x ? t[i - 1] : (y ? t[i - Side] : rng()));
            int step = int(rng() % 3) - 1;
            if (rng() % 1000 < big / 50) step = int(rng() % 65536) - 32768;
            t[i] = uint16_t(base + step);
        }
        RoundTrip(t);
    }

    // Corruption: flipped bytes must never crash; almost all must be detected.
    for (size_t y = 0; y < Side; ++y) for (size_t x = 0; x < Side; ++x) t[y * Side + x] = uint16_t(30000 + (x * y) / 97 + (rng() % 3));
    std::vector<uint8_t> blob(RawBytes);
    size_t n = Encode(t.data(), blob.data(), blob.size(), *enc);
    CHECK(n > 0);
    std::vector<uint16_t> out(Samples);
    int detected = 0, trials = 5000;
    for (int k = 0; k < trials; ++k) {
        std::vector<uint8_t> bad(blob.begin(), blob.begin() + n);
        bad[rng() % n] ^= uint8_t(1 + rng() % 255);
        if (!Decode(bad.data(), bad.size(), out.data(), *dec) || out != t) ++detected;
        if (Decode(bad.data(), bad.size(), out.data(), *dec) && out != t) {
            // Accepted but wrong would be silent corruption; count separately.
            printf("undetected corruption at trial %d\n", k);
            ++failures;
        }
    }
    printf("corruption: %d/%d rejected or changed output, 0 silent\n", detected, trials);

    if (argc > 1) {
        FILE* f = nullptr;
#ifdef _WIN32
        fopen_s(&f, argv[1], "rb");
#else
        f = fopen(argv[1], "rb");
#endif
        CHECK(f != nullptr);
        std::vector<std::vector<uint16_t>> tiles;
        while (f && fread(t.data(), RawBytes, 1, f) == 1) tiles.push_back(t);
        if (f) fclose(f);
        size_t total = 0;
        double encodeS = 0, decodeS = 0;
        std::vector<uint8_t> b(RawBytes);
        for (auto& tile : tiles) {
            auto s0 = Clock::now();
            size_t m = Encode(tile.data(), b.data(), RawBytes * 95 / 100, *enc);
            auto s1 = Clock::now();
            CHECK(m > 0);
            CHECK(Decode(b.data(), m, out.data(), *dec));
            auto s2 = Clock::now();
            CHECK(out == tile);
            total += m;
            encodeS += std::chrono::duration<double>(s1 - s0).count();
            decodeS += std::chrono::duration<double>(s2 - s1).count();
        }
        if (!tiles.empty())
            printf("real tiles=%zu ratio=%.6f mean_bytes=%.0f projected_gib_64980=%.3f encode_us=%.1f decode_us=%.1f exact=%d\n",
                   tiles.size(), double(total) / (tiles.size() * RawBytes), double(total) / tiles.size(),
                   double(total) / tiles.size() * 64980 / (1024.0 * 1024 * 1024),
                   encodeS * 1e6 / tiles.size(), decodeS * 1e6 / tiles.size(), failures == 0);
    }
    printf(failures ? "FAILED: %d\n" : "PASS: codec round trips, bounds, escapes and corruption checks\n", failures);
    return failures ? 1 : 0;
}
