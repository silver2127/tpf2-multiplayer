// Lossless codec for variable-length uint16 height blocks (the terrain
// alignment pass's per-region result and work vectors, 1..MaxSamples samples).
//
// The tile codec (terrain_codec.h) predicts from the row above, which needs the
// block's width; these blocks arrive as a bare sample count, so the predictor
// is the previous sample only. Everything else is the tile codec's proven
// machinery, reused by name: zigzag residuals, one static table per block,
// escapes for residuals >= 255, two interleaved rANS states with 16-bit
// renormalisation, a 64-bit content hash checked after decode.
//
// Pure functions, no allocation, bounded reads and writes; Decode is called
// from an access-violation handler.
#pragma once
#include "terrain_codec.h"

namespace BlockCodec {
constexpr size_t MaxSamples = 72000;   // above one tile's 66,049; the refine's 268x268 is 71,824
constexpr uint8_t Version = 1;
constexpr size_t HeaderBytes = 1 + 4 + 8 + 4 + 4;
using TerrainCodec::Scale; using TerrainCodec::ScaleBits; using TerrainCodec::Symbols;
using TerrainCodec::EscapeSymbol; using TerrainCodec::FreqShift; using TerrainCodec::CumShift;
using TerrainCodec::RansLow; using TerrainCodec::Table; using TerrainCodec::EncSymbol;

struct EncodeScratch {
    uint16_t z[MaxSamples];
    uint16_t escapes[MaxSamples];
    uint32_t counts[Symbols];
    Table table;
    EncSymbol symbols[Symbols];
    uint8_t stream[MaxSamples * 2 + 64];
};
struct DecodeScratch { uint32_t entry[Scale]; };

inline uint64_t Hash(const uint16_t* v, size_t n) {
    constexpr uint64_t K1 = 0x9E3779B185EBCA87ull, K2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t a = 0x27D4EB2F165667C5ull, b = 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w0, w1;
        memcpy(&w0, v + i, 8); memcpy(&w1, v + i + 4, 8);
        a = TerrainCodec::Rotl(a ^ (w0 * K2), 31) * K1;
        b = TerrainCodec::Rotl(b ^ (w1 * K1), 29) * K2;
    }
    for (; i < n; ++i) a = TerrainCodec::Rotl(a ^ (uint64_t(v[i]) * K2), 31) * K1;
    uint64_t h = a ^ TerrainCodec::Rotl(b, 17) ^ n;
    h ^= h >> 33; h *= K2; h ^= h >> 29;
    return h;
}

// Returns the encoded size, or 0 if it would not fit in `cap` bytes or n is
// out of range. Never writes past out[cap-1].
inline size_t Encode(const uint16_t* src, size_t n, uint8_t* out, size_t cap, EncodeScratch& w) {
    if (!n || n > MaxSamples) return 0;
    memset(w.counts, 0, sizeof w.counts);
    size_t escapes = 0;
    uint16_t prev = 0;
    for (size_t i = 0; i < n; ++i) {
        uint16_t z = TerrainCodec::Zig(uint16_t(src[i] - prev));
        prev = src[i];
        w.z[i] = z;
        unsigned sym = z < EscapeSymbol ? z : EscapeSymbol;
        ++w.counts[sym];
        if (sym == EscapeSymbol) w.escapes[escapes++] = z;
    }
    if (HeaderBytes + escapes * 2 >= cap) return 0;
    if (!TerrainCodec::Normalize(w.counts, w.table)) return 0;
    for (unsigned s = 0; s < Symbols; ++s)
        if (w.table.freq[s]) TerrainCodec::InitSymbol(w.symbols[s], w.table.cum[s], w.table.freq[s]);
    uint8_t* const end = w.stream + sizeof w.stream;
    uint8_t* p = end;
    uint32_t state[2] = {RansLow, RansLow};
    for (size_t i = n; i-- > 0;) {
        uint16_t z = w.z[i];
        const EncSymbol& e = w.symbols[z < EscapeSymbol ? z : EscapeSymbol];
        uint32_t st = state[i & 1];
        if (st >= e.limit) {
            if (p - w.stream < 2) return 0;
            p -= 2;
            p[0] = uint8_t(st); p[1] = uint8_t(st >> 8);
            st >>= 16;
        }
        uint32_t q = uint32_t((uint64_t(st) * e.reciprocal) >> e.shift);
        state[i & 1] = st + e.bias + q * e.complement;
    }
    if (size_t(p - w.stream) < 8) return 0;
    for (int k = 1; k >= 0; --k) {
        p -= 4;
        p[0] = uint8_t(state[k]); p[1] = uint8_t(state[k] >> 8); p[2] = uint8_t(state[k] >> 16); p[3] = uint8_t(state[k] >> 24);
    }
    size_t ransBytes = size_t(end - p);
    unsigned present = 0;
    for (unsigned s = 0; s < Symbols; ++s) if (w.table.freq[s]) ++present;
    size_t need = HeaderBytes + 2 + size_t(present) * 3 + escapes * 2 + ransBytes;
    if (need > cap) return 0;
    uint8_t* o = out;
    *o++ = Version;
    TerrainCodec::Put32(o, uint32_t(n));
    TerrainCodec::Put64(o, Hash(src, n));
    TerrainCodec::Put32(o, uint32_t(escapes));
    TerrainCodec::Put32(o, uint32_t(ransBytes));
    TerrainCodec::Put16(o, present);
    for (unsigned s = 0; s < Symbols; ++s) {
        if (!w.table.freq[s]) continue;
        *o++ = uint8_t(s);
        TerrainCodec::Put16(o, w.table.freq[s]);
    }
    for (size_t e = 0; e < escapes; ++e) TerrainCodec::Put16(o, w.escapes[e]);
    memcpy(o, p, ransBytes);
    o += ransBytes;
    return size_t(o - out);
}

// The sample count stored in a blob (0 if the header is malformed).
inline size_t StoredCount(const uint8_t* in, size_t len) {
    if (len < HeaderBytes || in[0] != Version) return 0;
    uint32_t n = 0; for (int i = 0; i < 4; ++i) n |= uint32_t(in[1 + i]) << (8 * i);
    return n <= MaxSamples ? n : 0;
}

// Decodes exactly `n` samples (which must equal the stored count) into dst.
// Returns false on any inconsistency; dst contents are then unspecified.
inline bool Decode(const uint8_t* in, size_t len, uint16_t* dst, size_t n, DecodeScratch& r) {
    const uint8_t* p = in;
    const uint8_t* const end = in + len;
    auto has = [&](size_t k) { return size_t(end - p) >= k; };
    auto get16 = [&]() { unsigned v = unsigned(p[0]) | (unsigned(p[1]) << 8); p += 2; return v; };
    auto get32 = [&]() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i); p += 4; return v; };
    if (!has(HeaderBytes) || *p++ != Version) return false;
    if (get32() != n || !n || n > MaxSamples) return false;
    uint64_t hash = uint64_t(get32());
    hash |= uint64_t(get32()) << 32;
    uint32_t escapes = get32(), ransBytes = get32();
    if (escapes > n || !has(2)) return false;
    unsigned present = get16();
    if (present > Symbols || !has(size_t(present) * 3)) return false;
    int previous = -1;
    unsigned cum = 0;
    for (unsigned k = 0; k < present; ++k) {
        unsigned s = *p++;
        unsigned f = get16();
        if (int(s) <= previous || f == 0 || f > Scale - cum) return false;
        previous = int(s);
        uint32_t v = uint32_t(s) | (uint32_t(f - 1) << FreqShift) | (uint32_t(cum) << CumShift);
        for (unsigned m = 0; m < f; ++m) r.entry[cum + m] = v;
        cum += f;
    }
    if (!present || cum != Scale) return false;
    if (!has(size_t(escapes) * 2)) return false;
    const uint8_t* const esc = p;
    p += size_t(escapes) * 2;
    if (ransBytes < 8 || size_t(end - p) != ransBytes) return false;
    uint32_t sa = get32(), sb = get32();
    const uint8_t* q = p;
    uint32_t e = 0;
    uint16_t value = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned m = sa & (Scale - 1);
        uint32_t v = r.entry[m];
        unsigned z = v & 0xff;
        sa = (((v >> FreqShift) & (Scale - 1)) + 1) * (sa >> ScaleBits) + m - (v >> CumShift);
        if (sa < RansLow) {
            if (end - q < 2) return false;
            sa = (sa << 16) | uint32_t(q[0]) | (uint32_t(q[1]) << 8);
            q += 2;
        }
        const uint32_t swap = sa; sa = sb; sb = swap;
        if (z == EscapeSymbol) {
            if (e >= escapes) return false;
            z = unsigned(esc[2 * e]) | (unsigned(esc[2 * e + 1]) << 8);
            if (z < EscapeSymbol) return false;
            ++e;
        }
        value = uint16_t(value + TerrainCodec::Unzig(uint16_t(z)));
        dst[i] = value;
    }
    return q == end && e == escapes && sa == RansLow && sb == RansLow && Hash(dst, n) == hash;
}
}  // namespace BlockCodec
