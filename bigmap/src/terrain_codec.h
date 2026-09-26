// Lossless codec for CTerrain's 257x257 uint16 height tiles (pager format 3).
//
// Planar prediction (left + up - upper-left, modulo 2^16), zigzag residuals and
// a static per-tile rANS coder with four neighbour contexts. On 1,024 real tiles
// the residuals are {0, -1, +1} for ~99.3% of samples (heights are stored in
// 5 cm steps), which is why an entropy coder beats LZ4 on nibble planes (about
// 7% of raw size versus 18.06% for format 2).
//
// Speed (measured on the same tiles; see tools/decode_experiments.cpp):
// - The decode table must fit in L1. At 12-bit precision it was 64 KiB and the
//   L2 load on each state's update chain bounded decoding to ~2.3 ns/sample, so
//   precision is 10 bits (16 KiB).
// - Two interleaved rANS states (even samples use state 0, odd samples state 1)
//   share one stream and live in registers, swapped per sample.
// - 16-bit renormalisation (state in [2^15, 2^31)): at most one read per
//   sample, and about half as many unpredictable renormalisation branches as
//   byte-wise output.
// - Each slot has a packed 32-bit entry (symbol, frequency-1, cumulative) and
//   the four context candidates of a slot are adjacent, loaded before the
//   context is known and selected with masks.
// - Unused contexts hold a sentinel entry instead of a per-sample check, and
//   stream bounds are checked once per row (a row consumes <= 2 bytes/sample).
//
// Integrity: a 64-bit hash of the raw tile is stored and checked after decode.
// Table symbol substitution and escape-value damage otherwise decode
// "consistently" into wrong heights; the pager must fail rather than invent
// terrain.
//
// Pure functions: no allocation, no globals, no engine calls, bounded reads and
// writes. The pager calls Decode from its access-violation handler, so it must
// stay that way. Blobs live only in memory (never saved), so the format has no
// compatibility obligation beyond one process.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace TerrainCodec {
constexpr size_t Side = 257, Samples = Side * Side, RawBytes = Samples * 2;
constexpr unsigned ScaleBits = 10, Scale = 1u << ScaleBits;
constexpr unsigned Contexts = 4, Symbols = 256, EscapeSymbol = 255;
constexpr unsigned FreqShift = 8, CumShift = 8 + ScaleBits;
// State interval [2^15, 2^31): the reciprocal (division-free) encoder update is
// exact only for states below 2^31, so a [2^16, 2^32) window mis-encodes.
constexpr uint32_t RansLow = 1u << 15;
constexpr uint8_t Version = 3;
constexpr size_t HeaderBytes = 1 + 8 + 4 + 4;

struct Table { uint16_t freq[Symbols], cum[Symbols]; };
// Division-free encoder symbol (reciprocal form of the rANS update).
struct EncSymbol { uint64_t limit; uint32_t reciprocal, bias; uint16_t complement, shift; };
// ~430 KiB. Allocate once; never on the stack.
struct EncodeScratch {
    uint16_t z[Samples];
    uint16_t escapes[Samples];
    uint32_t counts[Contexts][Symbols];
    Table tables[Contexts];
    EncSymbol symbols[Contexts][Symbols];
    uint8_t stream[RawBytes + 64];
};
// ~17 KiB. entry[slot << 2 | ctx] = symbol | (freq - 1) << FreqShift | cum << CumShift.
struct DecodeScratch {
    uint32_t entry[Scale * Contexts];
    uint16_t rows[Side];
};

// Context = bucket of (left residual + upper residual): 0, 1, 2-4, >=5.
inline unsigned ContextOf(unsigned left, unsigned up) {
    unsigned s = left + up;
    return unsigned(s != 0) + unsigned(s > 1) + unsigned(s > 4);
}
inline uint16_t Zig(uint16_t d) { return uint16_t((unsigned(d) << 1) ^ unsigned(uint16_t(int16_t(d) >> 15))); }
inline uint16_t Unzig(uint16_t z) { return uint16_t((unsigned(z) >> 1) ^ (0u - (unsigned(z) & 1u))); }
inline uint16_t Predict(const uint16_t* v, size_t x, size_t y) {
    size_t i = y * Side + x;
    if (y == 0) return x ? v[i - 1] : 0;
    if (x == 0) return v[i - Side];
    return uint16_t(v[i - 1] + v[i - Side] - v[i - Side - 1]);
}
inline uint64_t Rotl(uint64_t v, int k) { return (v << k) | (v >> (64 - k)); }
// Two independent multiply-rotate lanes over 4-sample words.
inline uint64_t Hash(const uint16_t* v) {
    constexpr uint64_t K1 = 0x9E3779B185EBCA87ull, K2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t a = 0x27D4EB2F165667C5ull, b = 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 8 <= Samples; i += 8) {
        uint64_t w0, w1;
        memcpy(&w0, v + i, 8);
        memcpy(&w1, v + i + 4, 8);
        a = Rotl(a ^ (w0 * K2), 31) * K1;
        b = Rotl(b ^ (w1 * K1), 29) * K2;
    }
    for (; i < Samples; ++i) a = Rotl(a ^ (uint64_t(v[i]) * K2), 31) * K1;
    uint64_t h = a ^ Rotl(b, 17) ^ Samples;
    h ^= h >> 33; h *= K2; h ^= h >> 29;
    return h;
}

// Scale counts to frequencies summing to exactly Scale; every present symbol >= 1.
inline bool Normalize(const uint32_t* counts, Table& t) {
    memset(&t, 0, sizeof t);
    uint64_t total = 0;
    unsigned best = 0;
    for (unsigned s = 0; s < Symbols; ++s) {
        total += counts[s];
        if (counts[s] > counts[best]) best = s;
    }
    if (!total) return true;
    unsigned sum = 0;
    for (unsigned s = 0; s < Symbols; ++s) {
        if (!counts[s]) continue;
        uint64_t f = (uint64_t(counts[s]) * Scale) / total;
        if (!f) f = 1;
        t.freq[s] = uint16_t(f);
        sum += unsigned(f);
    }
    if (sum < Scale) t.freq[best] = uint16_t(t.freq[best] + (Scale - sum));
    while (sum > Scale) {
        unsigned m = 0;
        for (unsigned s = 1; s < Symbols; ++s) if (t.freq[s] > t.freq[m]) m = s;
        if (t.freq[m] <= 1) return false;
        --t.freq[m];
        --sum;
    }
    unsigned c = 0;
    for (unsigned s = 0; s < Symbols; ++s) { t.cum[s] = uint16_t(c); c += t.freq[s]; }
    return c == Scale;
}

// x' = (x / f) * Scale + (x % f) + cum, computed as x + bias + q * (Scale - f)
// with q = floor(x / f) from a 64-bit reciprocal (the rans_byte formulation by
// F. Giesen; freq == 1 uses an all-ones reciprocal and a bias of Scale - 1).
// The renormalisation limit is 64-bit: for freq == Scale it is exactly 2^32.
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

inline void Put16(uint8_t*& p, unsigned v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p += 2; }
inline void Put32(uint8_t*& p, uint32_t v) { for (int i = 0; i < 4; ++i) *p++ = uint8_t(v >> (8 * i)); }
inline void Put64(uint8_t*& p, uint64_t v) { for (int i = 0; i < 8; ++i) *p++ = uint8_t(v >> (8 * i)); }

// Returns the encoded size, or 0 if it would not fit in `cap` bytes (the caller
// then keeps the tile uncompressed). Never writes past out[cap-1].
inline size_t Encode(const uint16_t* src, uint8_t* out, size_t cap, EncodeScratch& w) {
    memset(w.counts, 0, sizeof w.counts);
    size_t escapes = 0;
    for (size_t y = 0; y < Side; ++y) {
        for (size_t x = 0; x < Side; ++x) {
            size_t i = y * Side + x;
            uint16_t z = Zig(uint16_t(src[i] - Predict(src, x, y)));
            w.z[i] = z;
            unsigned ctx = ContextOf(x ? w.z[i - 1] : 0, y ? w.z[i - Side] : 0);
            unsigned sym = z < EscapeSymbol ? z : EscapeSymbol;
            ++w.counts[ctx][sym];
            if (sym == EscapeSymbol) w.escapes[escapes++] = z;
        }
    }
    if (HeaderBytes + escapes * 2 >= cap) return 0;
    for (unsigned c = 0; c < Contexts; ++c) {
        if (!Normalize(w.counts[c], w.tables[c])) return 0;
        for (unsigned s = 0; s < Symbols; ++s)
            if (w.tables[c].freq[s]) InitSymbol(w.symbols[c][s], w.tables[c].cum[s], w.tables[c].freq[s]);
    }

    // Interleaved rANS: sample i uses state i & 1. Encode the last sample first,
    // writing one shared stream of little-endian 16-bit words backwards; the
    // decoder mirrors it forwards.
    uint8_t* const end = w.stream + sizeof w.stream;
    uint8_t* p = end;
    uint32_t state[2] = {RansLow, RansLow};
    for (size_t i = Samples; i-- > 0;) {
        size_t x = i % Side, y = i / Side;
        uint16_t z = w.z[i];
        unsigned sym = z < EscapeSymbol ? z : EscapeSymbol;
        const EncSymbol& e = w.symbols[ContextOf(x ? w.z[i - 1] : 0, y ? w.z[i - Side] : 0)][sym];
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
    // Flush state 1 then state 0, so the decoder reads state 0 first.
    if (size_t(p - w.stream) < 8) return 0;
    for (int k = 1; k >= 0; --k) {
        p -= 4;
        p[0] = uint8_t(state[k]); p[1] = uint8_t(state[k] >> 8); p[2] = uint8_t(state[k] >> 16); p[3] = uint8_t(state[k] >> 24);
    }
    size_t ransBytes = size_t(end - p);

    size_t need = HeaderBytes + escapes * 2 + ransBytes;
    for (unsigned c = 0; c < Contexts; ++c) {
        need += 2;
        for (unsigned s = 0; s < Symbols; ++s) if (w.tables[c].freq[s]) need += 3;
    }
    if (need > cap) return 0;

    uint8_t* o = out;
    *o++ = Version;
    Put64(o, Hash(src));
    Put32(o, uint32_t(escapes));
    Put32(o, uint32_t(ransBytes));
    for (unsigned c = 0; c < Contexts; ++c) {
        unsigned present = 0;
        for (unsigned s = 0; s < Symbols; ++s) if (w.tables[c].freq[s]) ++present;
        Put16(o, present);
        for (unsigned s = 0; s < Symbols; ++s) {
            if (!w.tables[c].freq[s]) continue;
            *o++ = uint8_t(s);
            Put16(o, w.tables[c].freq[s]);
        }
    }
    for (size_t e = 0; e < escapes; ++e) Put16(o, w.escapes[e]);
    memcpy(o, p, ransBytes);
    o += ransBytes;
    return size_t(o - out);
}

namespace detail {
struct Cursor {
    uint32_t sa, sb;             // state for the current sample, then the next
    const uint8_t* q;            // next stream byte
    uint32_t e;                  // escapes consumed
};
// One row. With Checked=false the caller guarantees >= 2*Side readable stream
// bytes, which covers the at-most-one 16-bit renormalisation read per sample.
template <bool Checked>
inline bool DecodeRow(size_t y, uint16_t* dst, uint16_t* upz, const uint32_t* table, const uint8_t* end,
                      const uint8_t* esc, uint32_t escapes, Cursor& c) {
    uint32_t sa = c.sa, sb = c.sb, e = c.e;
    const uint8_t* q = c.q;
    uint16_t* const row = dst + y * Side;
    const uint16_t* const up = y ? row - Side : row;
    const uint32_t upMask = y ? ~0u : 0u;
    unsigned left = 0;            // residual of the previous sample in this row
    uint16_t value = 0;           // decoded value of the previous sample in this row
    for (size_t x = 0; x < Side; ++x) {
        unsigned m = sa & (Scale - 1);
        const uint32_t* const group = table + (size_t(m) << 2);
        uint32_t v = group[0];
        const uint32_t v1 = group[1], v2 = group[2], v3 = group[3];
        unsigned s = left + (upz[x] & upMask);
        const uint32_t k1 = 0u - uint32_t(s != 0), k2 = 0u - uint32_t(s > 1), k3 = 0u - uint32_t(s > 4);
        v = (v & ~k1) | (v1 & k1);
        v = (v & ~k2) | (v2 & k2);
        v = (v & ~k3) | (v3 & k3);
        unsigned z = v & 0xff;
        sa = (((v >> FreqShift) & (Scale - 1)) + 1) * (sa >> ScaleBits) + m - (v >> CumShift);
        // A valid update leaves sa >= 2^5, so one 16-bit word restores >= 2^21.
        if (sa < RansLow) {
            if (Checked && end - q < 2) return false;
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
        upz[x] = uint16_t(z);
        left = z;
        uint16_t pred = y ? (x ? uint16_t(value + up[x] - up[x - 1]) : up[0]) : value;
        value = uint16_t(pred + Unzig(uint16_t(z)));
        row[x] = value;
    }
    c.sa = sa; c.sb = sb; c.q = q; c.e = e;
    return true;
}
}  // namespace detail

// Decodes exactly Samples values into dst. Returns false on any inconsistency
// (bounds, table sums, escape count, trailing bytes, final rANS states or the
// content hash); dst contents are then unspecified. Reads nothing outside
// in[0..n).
inline bool Decode(const uint8_t* in, size_t n, uint16_t* dst, DecodeScratch& r, bool verifyHash = true) {
    const uint8_t* p = in;
    const uint8_t* const end = in + n;
    auto has = [&](size_t k) { return size_t(end - p) >= k; };
    auto get16 = [&]() { unsigned v = unsigned(p[0]) | (unsigned(p[1]) << 8); p += 2; return v; };
    auto get32 = [&]() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i); p += 4; return v; };

    if (!has(HeaderBytes) || *p++ != Version) return false;
    uint64_t hash = uint64_t(get32());
    hash |= uint64_t(get32()) << 32;
    uint32_t escapes = get32(), ransBytes = get32();
    if (escapes > Samples) return false;
    // A context no sample uses holds a sentinel: an escape with freq=Scale and
    // cum=0 leaves the state unchanged and fails on the escape bound. A valid
    // stream never selects it (every selected context has a count).
    constexpr uint32_t Sentinel = EscapeSymbol | ((Scale - 1) << FreqShift);
    for (unsigned c = 0; c < Contexts; ++c) {
        if (!has(2)) return false;
        unsigned present = get16();
        if (present > Symbols || !has(size_t(present) * 3)) return false;
        int previous = -1;
        unsigned cum = 0;
        for (unsigned k = 0; k < present; ++k) {
            unsigned s = *p++;
            unsigned f = get16();
            if (int(s) <= previous || f == 0 || f > Scale - cum) return false;
            previous = int(s);
            // Symbols arrive in ascending order, so cum matches the encoder's.
            uint32_t v = uint32_t(s) | (uint32_t(f - 1) << FreqShift) | (uint32_t(cum) << CumShift);
            for (unsigned m = 0; m < f; ++m) r.entry[((cum + m) << 2) | c] = v;
            cum += f;
        }
        if (present && cum != Scale) return false;
        if (!present) for (unsigned m = 0; m < Scale; ++m) r.entry[(m << 2) | c] = Sentinel;
    }
    if (!has(size_t(escapes) * 2)) return false;
    const uint8_t* const esc = p;
    p += size_t(escapes) * 2;
    if (ransBytes < 8 || size_t(end - p) != ransBytes) return false;
    detail::Cursor cursor;
    cursor.sa = get32();
    cursor.sb = get32();
    cursor.q = p;
    cursor.e = 0;

    // rows[] holds the previous row's residuals; it is overwritten in place as
    // the current row is decoded (index x is read before it is written).
    for (size_t y = 0; y < Side; ++y) {
        bool ok = size_t(end - cursor.q) >= 2 * Side
            ? detail::DecodeRow<false>(y, dst, r.rows, r.entry, end, esc, escapes, cursor)
            : detail::DecodeRow<true>(y, dst, r.rows, r.entry, end, esc, escapes, cursor);
        if (!ok) return false;
    }
    return cursor.q == end && cursor.e == escapes && cursor.sa == RansLow && cursor.sb == RansLow &&
           (!verifyHash || Hash(dst) == hash);
}
}  // namespace TerrainCodec
