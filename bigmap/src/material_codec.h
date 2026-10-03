// Lossless codec for the terrain renderer's material-index cells (Steam 35924):
// one std::vector<uint8> of 260x260 material indices plus a canary byte per tile
// (DataGrid<uint8> at RenderDataManager+0x250; see docs/runtime-memory-audit.md).
//
// The bytes are dithered between a few ground materials per tile (median 8
// distinct values, max 19 in 1,024 live samples), so runs are short and LZ-style
// coders reach only ~24% (zstd). This codec maps each cell to its own dense
// alphabet (<= 64 values) and codes each byte with a static per-cell rANS model
// conditioned on the previous byte (the byte above at a row start), which a
// Python estimate put at ~20% of raw size.
//
// Core shared with terrain_codec.h: two interleaved rANS states, 16-bit
// renormalisation in [2^15, 2^31) (the division-free encoder update is exact
// only below 2^31), 10-bit precision, and a 64-bit content hash checked after
// decode. Pure functions: no allocation, no globals, bounded reads and writes,
// safe to call from an access-violation handler. In-memory format only.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace MaterialCodec {
constexpr size_t Width = 260, Bytes = Width * Width + 1;
constexpr unsigned MaxAlphabet = 64, ScaleBits = 10, Scale = 1u << ScaleBits;
constexpr unsigned FreqShift = 8, CumShift = 8 + ScaleBits;
constexpr uint32_t RansLow = 1u << 15;
constexpr uint8_t Version = 1;

struct EncSymbol { uint64_t limit; uint32_t reciprocal, bias; uint16_t complement, shift; };
// ~250 KiB. Allocate once; never on the stack.
struct EncodeScratch {
    uint8_t sym[Bytes];
    uint32_t counts[MaxAlphabet][MaxAlphabet];
    uint16_t freq[MaxAlphabet][MaxAlphabet];
    uint16_t cum[MaxAlphabet][MaxAlphabet];
    EncSymbol enc[MaxAlphabet][MaxAlphabet];
    uint8_t stream[Bytes + 64];
};
// ~250 KiB (only alphabet-size * 4 KiB of it is touched per cell).
// entry[ctx * Scale + slot] = symbol | (freq - 1) << FreqShift | cum << CumShift.
struct DecodeScratch {
    uint32_t entry[MaxAlphabet * Scale];
};

inline uint64_t Rotl(uint64_t v, int k) { return (v << k) | (v >> (64 - k)); }
inline uint64_t Hash(const uint8_t* v) {
    constexpr uint64_t K1 = 0x9E3779B185EBCA87ull, K2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t a = 0x27D4EB2F165667C5ull, b = 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 16 <= Bytes; i += 16) {
        uint64_t w0, w1;
        memcpy(&w0, v + i, 8);
        memcpy(&w1, v + i + 8, 8);
        a = Rotl(a ^ (w0 * K2), 31) * K1;
        b = Rotl(b ^ (w1 * K1), 29) * K2;
    }
    for (; i < Bytes; ++i) a = Rotl(a ^ (uint64_t(v[i]) * K2), 31) * K1;
    uint64_t h = a ^ Rotl(b, 17) ^ Bytes;
    h ^= h >> 33; h *= K2; h ^= h >> 29;
    return h;
}
// Context of byte i: the previous byte, or the first byte of the previous row at
// a row start (the canary at index 260*260 is a row start too).
inline unsigned ContextAt(const uint8_t* sym, size_t i) {
    return i == 0 ? 0u : (i % Width == 0 ? sym[i - Width] : sym[i - 1]);
}

inline bool Normalize(const uint32_t* counts, unsigned symbols, uint16_t* freq, uint16_t* cum) {
    memset(freq, 0, sizeof(uint16_t) * MaxAlphabet);
    memset(cum, 0, sizeof(uint16_t) * MaxAlphabet);
    uint64_t total = 0;
    unsigned best = 0;
    for (unsigned s = 0; s < symbols; ++s) {
        total += counts[s];
        if (counts[s] > counts[best]) best = s;
    }
    if (!total) return true;
    unsigned sum = 0;
    for (unsigned s = 0; s < symbols; ++s) {
        if (!counts[s]) continue;
        uint64_t f = (uint64_t(counts[s]) * Scale) / total;
        if (!f) f = 1;
        freq[s] = uint16_t(f);
        sum += unsigned(f);
    }
    if (sum < Scale) freq[best] = uint16_t(freq[best] + (Scale - sum));
    while (sum > Scale) {
        unsigned m = 0;
        for (unsigned s = 1; s < symbols; ++s) if (freq[s] > freq[m]) m = s;
        if (freq[m] <= 1) return false;
        --freq[m];
        --sum;
    }
    unsigned c = 0;
    for (unsigned s = 0; s < symbols; ++s) { cum[s] = uint16_t(c); c += freq[s]; }
    return c == Scale;
}

inline void InitSymbol(EncSymbol& e, unsigned cum, unsigned freq) {
    e.limit = (uint64_t(RansLow >> ScaleBits) << 16) * freq;
    e.complement = uint16_t(Scale - freq);
    if (freq < 2) {
        e.reciprocal = ~0u;
        e.shift = 32;
        e.bias = cum + Scale - 1;
    } else {
        unsigned shift = 0;
        while (freq > (1u << shift)) ++shift;
        e.reciprocal = uint32_t(((uint64_t(1) << (shift + 31)) + freq - 1) / freq);
        e.shift = uint16_t(shift - 1 + 32);
        e.bias = cum;
    }
}

// Returns the encoded size, or 0 if the cell has more than MaxAlphabet distinct
// values or would not fit in `cap` bytes. Never writes past out[cap-1].
inline size_t Encode(const uint8_t* src, uint8_t* out, size_t cap, EncodeScratch& w) {
    int lut[256];
    for (int& v : lut) v = -1;
    uint8_t alphabet[MaxAlphabet];
    unsigned symbols = 0;
    for (size_t i = 0; i < Bytes; ++i) {
        uint8_t b = src[i];
        if (lut[b] < 0) {
            if (symbols == MaxAlphabet) return 0;
            lut[b] = int(symbols);
            alphabet[symbols++] = b;
        }
        w.sym[i] = uint8_t(lut[b]);
    }
    memset(w.counts, 0, sizeof w.counts);
    for (size_t i = 0; i < Bytes; ++i) ++w.counts[ContextAt(w.sym, i)][w.sym[i]];
    for (unsigned c = 0; c < symbols; ++c) {
        if (!Normalize(w.counts[c], symbols, w.freq[c], w.cum[c])) return 0;
        for (unsigned s = 0; s < symbols; ++s)
            if (w.freq[c][s]) InitSymbol(w.enc[c][s], w.cum[c][s], w.freq[c][s]);
    }

    uint8_t* const end = w.stream + sizeof w.stream;
    uint8_t* p = end;
    uint32_t state[2] = {RansLow, RansLow};
    for (size_t i = Bytes; i-- > 0;) {
        const EncSymbol& e = w.enc[ContextAt(w.sym, i)][w.sym[i]];
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

    size_t need = 1 + 8 + 1 + symbols + 4 + ransBytes;
    for (unsigned c = 0; c < symbols; ++c) {
        need += 1;
        for (unsigned s = 0; s < symbols; ++s) if (w.freq[c][s]) need += 3;
    }
    if (need > cap) return 0;

    uint8_t* o = out;
    *o++ = Version;
    uint64_t h = Hash(src);
    for (int i = 0; i < 8; ++i) *o++ = uint8_t(h >> (8 * i));
    *o++ = uint8_t(symbols - 1);
    memcpy(o, alphabet, symbols);
    o += symbols;
    for (int i = 0; i < 4; ++i) *o++ = uint8_t(ransBytes >> (8 * i));
    for (unsigned c = 0; c < symbols; ++c) {
        unsigned present = 0;
        for (unsigned s = 0; s < symbols; ++s) if (w.freq[c][s]) ++present;
        *o++ = uint8_t(present);
        for (unsigned s = 0; s < symbols; ++s) {
            if (!w.freq[c][s]) continue;
            *o++ = uint8_t(s);
            *o++ = uint8_t(w.freq[c][s]);
            *o++ = uint8_t(w.freq[c][s] >> 8);
        }
    }
    memcpy(o, p, ransBytes);
    o += ransBytes;
    return size_t(o - out);
}

// Decodes exactly Bytes values into dst. Returns false on any inconsistency
// (bounds, tables, trailing bytes, final states, content hash). Reads nothing
// outside in[0..n); dst contents are unspecified on failure.
inline bool Decode(const uint8_t* in, size_t n, uint8_t* dst, DecodeScratch& r) {
    const uint8_t* p = in;
    const uint8_t* const end = in + n;
    auto has = [&](size_t k) { return size_t(end - p) >= k; };
    if (!has(1 + 8 + 1) || *p++ != Version) return false;
    uint64_t hash = 0;
    for (int i = 0; i < 8; ++i) hash |= uint64_t(p[i]) << (8 * i);
    p += 8;
    unsigned symbols = unsigned(*p++) + 1;
    if (symbols > MaxAlphabet || !has(symbols + 4)) return false;
    uint8_t alphabet[MaxAlphabet] = {};
    memcpy(alphabet, p, symbols);
    p += symbols;
    uint32_t ransBytes = 0;
    for (int i = 0; i < 4; ++i) ransBytes |= uint32_t(p[i]) << (8 * i);
    p += 4;
    for (unsigned c = 0; c < symbols; ++c) {
        if (!has(1)) return false;
        unsigned present = *p++;
        if (present > symbols || !has(size_t(present) * 3)) return false;
        int previous = -1;
        unsigned cum = 0;
        uint32_t* const entry = r.entry + c * Scale;
        for (unsigned k = 0; k < present; ++k) {
            unsigned s = p[0], f = unsigned(p[1]) | (unsigned(p[2]) << 8);
            p += 3;
            if (s >= symbols || int(s) <= previous || f == 0 || f > Scale - cum) return false;
            previous = int(s);
            uint32_t v = uint32_t(s) | (uint32_t(f - 1) << FreqShift) | (uint32_t(cum) << CumShift);
            for (unsigned m = 0; m < f; ++m) entry[cum + m] = v;
            cum += f;
        }
        if (present && cum != Scale) return false;
        // An empty context is never selected by a valid stream; give it a
        // harmless entry (symbol 0, freq Scale) so a corrupt one stays bounded.
        if (!present) for (unsigned m = 0; m < Scale; ++m) entry[m] = (Scale - 1) << FreqShift;
    }
    if (ransBytes < 8 || size_t(end - p) != ransBytes) return false;
    uint32_t sa = 0, sb = 0;
    for (int i = 0; i < 4; ++i) sa |= uint32_t(p[i]) << (8 * i);
    for (int i = 0; i < 4; ++i) sb |= uint32_t(p[4 + i]) << (8 * i);
    const uint8_t* q = p + 8;

    unsigned prev = 0, rowFirst = 0;
    for (size_t i = 0; i < Bytes; ++i) {
        const bool rowStart = i % Width == 0;
        unsigned ctx = rowStart ? rowFirst : prev;
        unsigned m = sa & (Scale - 1);
        uint32_t v = r.entry[ctx * Scale + m];
        unsigned s = v & 0xff;
        sa = (((v >> FreqShift) & (Scale - 1)) + 1) * (sa >> ScaleBits) + m - (v >> CumShift);
        if (sa < RansLow) {
            if (end - q < 2) return false;
            sa = (sa << 16) | uint32_t(q[0]) | (uint32_t(q[1]) << 8);
            q += 2;
        }
        const uint32_t swap = sa; sa = sb; sb = swap;
        if (s >= symbols) return false;
        if (rowStart) rowFirst = s;
        prev = s;
        dst[i] = alphabet[s];
    }
    return q == end && sa == RansLow && sb == RansLow && Hash(dst) == hash;
}
}  // namespace MaterialCodec
