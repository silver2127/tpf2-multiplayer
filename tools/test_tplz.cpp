// tools\test_tplz.cpp -- the terrain wire codec (native\src\slice\tplz.h):
// round trips, the paint stroke measured in a player's logs, and a decoder fed
// damaged streams. Build and run: tools\test_tplz.bat
#include "../native/src/slice/tplz.h"
#include <stdio.h>
#include <vector>
#include <random>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static uint64_t RoundTrip(const std::vector<uint8_t>& in, const char* name)
{
    std::vector<uint8_t> z((size_t)TplzBound(in.size()));
    const uint64_t zn = TplzCompress(in.data(), in.size(), z.data());
    CHECK(zn <= TplzBound(in.size()));
    std::vector<uint8_t> back(in.size() + 1, 0xcd);
    const bool ok = TplzDecompress(z.data(), zn, back.data(), in.size());
    CHECK(ok);
    CHECK(memcmp(back.data(), in.data(), in.size()) == 0);
    CHECK(back[in.size()] == 0xcd);                        // nothing written past the end
    if (in.size()) CHECK(!TplzDecompress(z.data(), zn, back.data(), in.size() - 1));   // wrong size refused
    CHECK(!TplzDecompress(z.data(), zn, back.data(), in.size() + 1));
    if (name) printf("%-34s %9llu B -> %8llu B (%.2f%%)\n", name, (unsigned long long)in.size(),
                     (unsigned long long)zn, in.size() ? 100.0 * zn / in.size() : 0.0);
    return zn;
}

int main()
{
    std::mt19937 rng(12345);
    // edge sizes
    for (size_t n = 0; n < 40; n++) {
        std::vector<uint8_t> a(n);
        for (auto& b : a) b = (uint8_t)(rng() % 3);
        RoundTrip(a, nullptr);
        std::vector<uint8_t> r(n, 7);
        RoundTrip(r, nullptr);
    }
    // the paint stroke from the logs: 639 x 559 material, a round brush of id 82
    // over 0xff, an all-zero mask, a 0x100-byte header
    {
        const int w = 639, h = 559;
        std::vector<uint8_t> blob(8 + 0x80 + 0x70 + 24, 0);
        memcpy(blob.data(), "TPTG\1\0\0\0", 8);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const double dx = x - w / 2.0, dy = y - h / 2.0;
                const bool in = dx * dx / (w * w / 4.0) + dy * dy / (h * h / 4.0) < 0.36 + 0.02 * ((x * 7 + y * 13) % 5);
                blob.push_back(in ? 82 : 0xff);
            }
        blob.insert(blob.end(), ((size_t)w * h + 31) / 32 * 4, 0);
        const uint64_t zn = RoundTrip(blob, "paint stroke 639x559 + mask");
        CHECK(zn * 20 < blob.size());                       // at least 20x smaller
    }
    // a terraform stroke: {height, base} doubles-as-floats over a hill, changed in a disc
    {
        const int w = 180, h = 170;
        std::vector<uint8_t> blob;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const float base = 40.0f + 0.05f * x + 0.03f * y + 2.0f * (float)sin(x * 0.1) * (float)cos(y * 0.07);
                const double dx = x - w / 2.0, dy = y - h / 2.0;
                const float ht = dx * dx + dy * dy < 60.0 * 60.0 ? 55.0f : base;
                blob.insert(blob.end(), (uint8_t*)&ht, (uint8_t*)&ht + 4);
                blob.insert(blob.end(), (uint8_t*)&base, (uint8_t*)&base + 4);
            }
        RoundTrip(blob, "terraform stroke 180x170");
    }
    // incompressible: never more than the bound
    {
        std::vector<uint8_t> a(300000);
        for (auto& b : a) b = (uint8_t)rng();
        RoundTrip(a, "random 300 KB");
    }
    // mixed runs and repeats
    for (int t = 0; t < 200; t++) {
        std::vector<uint8_t> a;
        const size_t n = rng() % 20000;
        while (a.size() < n) {
            const int kind = rng() % 3;
            const size_t len = 1 + rng() % 300;
            if (kind == 0) a.insert(a.end(), len, (uint8_t)rng());
            else if (kind == 1 || a.size() < 8) for (size_t k = 0; k < len; k++) a.push_back((uint8_t)rng());
            else { const size_t off = 1 + rng() % a.size(); for (size_t k = 0; k < len; k++) a.push_back(a[a.size() - off]); }
        }
        RoundTrip(a, nullptr);
    }
    // damaged frames: refused, or (a byte overwritten with its own value) the
    // exact original -- never a different blob. A checked debug build
    // (tools\test_tplz.bat): the CRT debug heap catches a write past a buffer.
    {
        std::vector<uint8_t> a(50000);
        for (size_t k = 0; k < a.size(); k++) a[k] = (uint8_t)((k / 97) % 5 == 0 ? rng() : 0xff);
        uint64_t zn = 0;
        uint8_t* z = TplzPack(a.data(), a.size(), &zn);
        CHECK(z && TplzIsPacked(z, zn));
        const char* why = "";
        uint64_t rn = 0;
        uint8_t* r = TplzUnpack(z, zn, 1 << 20, &rn, &why);
        CHECK(r && rn == a.size() && memcmp(r, a.data(), a.size()) == 0);
        free(r);
        CHECK(!TplzUnpack(z, zn, a.size() - 1, &rn, &why));   // over the reader's limit
        int refused = 0, same = 0;
        for (int t = 0; t < 20000; t++) {
            std::vector<uint8_t> d(z, z + zn);
            const int edits = 1 + rng() % 4;
            for (int e = 0; e < edits; e++) d[TPLZ_FRAME + rng() % (d.size() - TPLZ_FRAME)] = (uint8_t)rng();
            if (rng() % 4 == 0) d.resize(TPLZ_FRAME + rng() % (d.size() - TPLZ_FRAME));
            uint8_t* back = TplzUnpack(d.data(), d.size(), 1 << 20, &rn, &why);
            if (!back) { refused++; continue; }
            CHECK(rn == a.size() && memcmp(back, a.data(), a.size()) == 0);
            same++;
            free(back);
        }
        printf("damaged frames: %d of 20000 refused, %d decoded to the original\n", refused, same);
        free(z);
        // a blob that does not shrink is not packed
        std::vector<uint8_t> noise(1000);
        for (auto& b : noise) b = (uint8_t)rng();
        CHECK(!TplzPack(noise.data(), noise.size(), &zn));
        CHECK(!TplzIsPacked((const uint8_t*)"TPTG\1\0\0\0", 8));
    }
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
