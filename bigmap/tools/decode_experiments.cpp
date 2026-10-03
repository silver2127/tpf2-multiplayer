// Where does TerrainCodec::Decode spend its time? Timing variants of the sample
// loop on real tiles. Offline only; not part of the plugin.
#include "../src/terrain_codec.h"
#include <windows.h>
#include <vector>
#include <cstdio>
#include <memory>

using namespace TerrainCodec;
static double Now() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return double(t.QuadPart) / f.QuadPart; }

struct Parsed { const uint8_t* esc; const uint8_t* stream; const uint8_t* end; uint32_t escapes; bool used[Contexts]; };

// Header/table parse copied from Decode (entry layout slot<<2|ctx).
static bool Parse(const uint8_t* in, size_t n, DecodeScratch& r, Parsed& out) {
    const uint8_t* p = in; const uint8_t* end = in + n;
    auto get16 = [&]() { unsigned v = unsigned(p[0]) | (unsigned(p[1]) << 8); p += 2; return v; };
    auto get32 = [&]() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i); p += 4; return v; };
    if (*p++ != Version) return false;
    get32(); get32();
    out.escapes = get32(); get32();
    for (unsigned c = 0; c < Contexts; ++c) {
        unsigned present = get16(), cum = 0;
        for (unsigned k = 0; k < present; ++k) {
            unsigned s = *p++, f = get16();
            uint32_t v = uint32_t(s) | (uint32_t(f - 1) << FreqShift) | (uint32_t(cum) << CumShift);
            for (unsigned m = 0; m < f; ++m) r.entry[((cum + m) << 2) | c] = v;
            cum += f;
        }
        out.used[c] = present != 0;
    }
    out.esc = p; p += out.escapes * 2; out.stream = p; out.end = end;
    return true;
}

// mode bits: 1 = write output/predictor, 2 = use context mask select (else ctx 0 only is WRONG
// but isolates the mux), 4 = branchy renorm (else single-byte cmov renorm, wrong output)
__declspec(noinline) static uint64_t Loop(const Parsed& ps, DecodeScratch& r, uint16_t* dst, int mode) {
    const uint8_t* q = ps.stream;
    uint32_t sa = 0, sb = 0;
    for (int i = 0; i < 4; ++i) sa |= uint32_t(q[i]) << (8 * i);
    for (int i = 0; i < 4; ++i) sb |= uint32_t(q[4 + i]) << (8 * i);
    q += 8;
    const uint32_t* table = r.entry;
    uint16_t* upz = r.rows;
    uint32_t e = 0;
    uint64_t acc = 0;
    for (size_t y = 0; y < Side; ++y) {
        uint16_t* row = dst + y * Side;
        const uint16_t* up = y ? row - Side : row;
        unsigned left = 0; uint16_t value = 0;
        for (size_t x = 0; x < Side; ++x) {
            unsigned m = sa & (Scale - 1);
            const uint32_t* g = table + (size_t(m) << 2);
            uint32_t v = g[0];
            if (mode & 2) {
                unsigned s = left + (y ? upz[x] : 0u);
                const uint32_t k1 = 0u - uint32_t(s != 0), k2 = 0u - uint32_t(s > 1), k3 = 0u - uint32_t(s > 4);
                v = (v & ~k1) | (g[1] & k1);
                v = (v & ~k2) | (g[2] & k2);
                v = (v & ~k3) | (g[3] & k3);
            }
            unsigned z = v & 0xff;
            sa = (((v >> FreqShift) & (Scale - 1)) + 1) * (sa >> ScaleBits) + m - (v >> CumShift);
            if (mode & 4) {
                if (sa < RansLow) { if (q < ps.end) sa = (sa << 8) | *q++; if (sa < RansLow && q < ps.end) sa = (sa << 8) | *q++; }
            } else {
                unsigned need = sa < RansLow && q < ps.end;
                sa = need ? (sa << 8) | *q : sa;
                q += need;
            }
            uint32_t sw = sa; sa = sb; sb = sw;
            if (z == EscapeSymbol && e < ps.escapes) { z = unsigned(ps.esc[2 * e]) | (unsigned(ps.esc[2 * e + 1]) << 8); ++e; }
            upz[x] = uint16_t(z);
            left = z;
            if (mode & 1) {
                uint16_t pred = y ? (x ? uint16_t(value + up[x] - up[x - 1]) : up[0]) : value;
                value = uint16_t(pred + Unzig(uint16_t(z)));
                row[x] = value;
            } else {
                acc += z;
            }
        }
    }
    return acc + (q - ps.stream);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = nullptr; fopen_s(&f, argv[1], "rb"); if (!f) return 2;
    std::vector<std::vector<uint16_t>> tiles; std::vector<uint16_t> t(Samples), out(Samples);
    while (fread(t.data(), RawBytes, 1, f) == 1) tiles.push_back(t);
    fclose(f);
    std::unique_ptr<EncodeScratch> es(new EncodeScratch);
    std::unique_ptr<DecodeScratch> ds(new DecodeScratch);
    std::vector<std::vector<uint8_t>> blobs;
    for (auto& tile : tiles) { std::vector<uint8_t> b(RawBytes); b.resize(Encode(tile.data(), b.data(), b.size(), *es)); blobs.push_back(b); }
    // Sanity: full decoder
    double a = Now();
    for (auto& b : blobs) if (!Decode(b.data(), b.size(), out.data(), *ds)) { printf("decode failed\n"); return 1; }
    printf("full Decode: %.1f us/tile\n", (Now() - a) * 1e6 / blobs.size());
    const char* names[] = {"mux0+nowrite+cmovrenorm", "write", "mux", "write+mux", "branchrenorm", "write+branchrenorm", "mux+branchrenorm", "write+mux+branchrenorm (= real loop)"};
    for (int mode = 0; mode < 8; ++mode) {
        double total = 0; uint64_t sink = 0;
        for (auto& b : blobs) {
            Parsed ps; Parse(b.data(), b.size(), *ds, ps);
            double s0 = Now();
            sink += Loop(ps, *ds, out.data(), mode);
            total += Now() - s0;
        }
        printf("mode %d %-40s loop %.1f us/tile (sink %llu)\n", mode, names[mode], total * 1e6 / blobs.size(), (unsigned long long)sink);
    }
    // Raw memory-touch baseline: write 66,049 uint16 per tile.
    a = Now();
    for (auto& b : blobs) { for (size_t i = 0; i < Samples; ++i) out[i] = uint16_t(i + b.size()); }
    printf("baseline write loop: %.1f us/tile\n", (Now() - a) * 1e6 / blobs.size());
}
