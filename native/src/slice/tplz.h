// tplz.h -- a small LZ77 codec for the terrain edit on the wire (2026-09-26).
// Header only, no platform calls: tpf2_slice.dll includes it, and so does
// tools\test_tplz.cpp.
//
// WHY: a paint stroke ships its whole bounding box. Measured in a player's logs
// (2026-09-26): 639 x 559 material cells, 72% of them 0xff ("unchanged") and
// the rest one material id, plus an all-zero mask -- 270-840 KB a stroke, 38 MB
// in one session. Through a relayed Steam link (0.5 MB/s measured) every stroke
// arrived after its stamp, the session crawled at 0.25x for minutes and the
// players' own station and asset placements waited behind it.
//
// Stream: repeated { varint literal count, the literals, varint match length }
// with a varint offset after every non-zero match length; a zero match length
// ends the stream. Matches are at least TPLZ_MIN_MATCH bytes and may overlap
// their source (offset 1 is a run). Varints are LEB128. The decoder checks
// every length and offset against both buffers.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t TPLZ_MIN_MATCH = 4;
static const int      TPLZ_HASH_BITS = 16;

static inline uint8_t* TplzPutVar(uint8_t* o, uint64_t v)
{
    while (v >= 0x80) { *o++ = (uint8_t)(v | 0x80); v >>= 7; }
    *o++ = (uint8_t)v;
    return o;
}

static inline bool TplzGetVar(const uint8_t** p, const uint8_t* end, uint64_t* v)
{
    uint64_t r = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *v = r; return true; }
    }
    return false;
}

// The worst case: one literal run of all n bytes, its count and the end marker.
static inline uint64_t TplzBound(uint64_t n) { return n + 24; }

// Compress n bytes into out (at least TplzBound(n) bytes); returns the length.
static uint64_t TplzCompress(const uint8_t* in, uint64_t n, uint8_t* out)
{
    uint8_t* o = out;
    uint64_t lit = 0, i = 0;
    int64_t* head = (int64_t*)malloc(sizeof(int64_t) << TPLZ_HASH_BITS);
    if (!head) {
        o = TplzPutVar(o, n); memcpy(o, in, (size_t)n); o += n;
        return (uint64_t)(TplzPutVar(o, 0) - out);
    }
    for (size_t k = 0; k < ((size_t)1 << TPLZ_HASH_BITS); k++) head[k] = -1;
    while (n >= TPLZ_MIN_MATCH && i + TPLZ_MIN_MATCH <= n) {
        uint32_t w;
        memcpy(&w, in + i, 4);
        const uint32_t h = (w * 2654435761u) >> (32 - TPLZ_HASH_BITS);
        const int64_t cand = head[h];
        head[h] = (int64_t)i;
        uint64_t len = 0, off = 0;
        // a run of one byte value is always worth taking at offset 1
        if (i > 0 && in[i - 1] == in[i]) {
            uint64_t l = 0;
            while (i + l < n && in[i + l] == in[i - 1]) l++;
            if (l >= TPLZ_MIN_MATCH) { len = l; off = 1; }
        }
        if (cand >= 0 && memcmp(in + cand, in + i, 4) == 0) {
            uint64_t l = 4;
            while (i + l < n && in[(uint64_t)cand + l] == in[i + l]) l++;
            if (l > len) { len = l; off = i - (uint64_t)cand; }
        }
        if (len < TPLZ_MIN_MATCH) { lit++; i++; continue; }
        o = TplzPutVar(o, lit);
        memcpy(o, in + i - lit, (size_t)lit); o += lit;
        o = TplzPutVar(o, len);
        o = TplzPutVar(o, off);
        // seed the table sparsely inside the match: enough for the next rows to
        // find it, cheap on a long run
        const uint64_t stop = i + len;
        for (uint64_t j = i + 1; j + 4 <= n && j < stop; j += (len > 64 ? 16 : 1)) {
            memcpy(&w, in + j, 4);
            head[(w * 2654435761u) >> (32 - TPLZ_HASH_BITS)] = (int64_t)j;
        }
        i = stop;
        lit = 0;
    }
    lit += n - i;
    o = TplzPutVar(o, lit);
    memcpy(o, in + n - lit, (size_t)lit); o += lit;
    o = TplzPutVar(o, 0);
    free(head);
    return (uint64_t)(o - out);
}

// Decompress into exactly outLen bytes. False on any malformed stream: a length
// or offset outside either buffer, a stream that ends early, or one that does
// not fill out exactly.
static bool TplzDecompress(const uint8_t* in, uint64_t n, uint8_t* out, uint64_t outLen)
{
    const uint8_t* p = in;
    const uint8_t* end = in + n;
    uint64_t o = 0;
    for (;;) {
        uint64_t lit, len, off;
        if (!TplzGetVar(&p, end, &lit)) return false;
        if (lit > (uint64_t)(end - p) || lit > outLen - o) return false;
        memcpy(out + o, p, (size_t)lit); p += lit; o += lit;
        if (!TplzGetVar(&p, end, &len)) return false;
        if (len == 0) return o == outLen && p == end;
        if (!TplzGetVar(&p, end, &off)) return false;
        if (len < TPLZ_MIN_MATCH || off == 0 || off > o || len > outLen - o) return false;
        const uint8_t* src = out + o - off;
        if (off >= len) memcpy(out + o, src, (size_t)len);
        else for (uint64_t k = 0; k < len; k++) out[o + k] = src[k];
        o += len;
    }
}

// FRAMING: "TPTG", u32 TPLZ_WIRE_PACKED, u64 the blob's length, u64 its FNV-1a,
// then the stream. The version sits where a version-1 TPTG blob keeps its own,
// so a reader tells the two apart by it.
static const uint32_t TPLZ_WIRE_PACKED = 2;
static const uint64_t TPLZ_FRAME = 24;

static inline uint64_t TplzFnv(const uint8_t* p, uint64_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// malloc'd frame, or nullptr when the blob does not shrink (ship it as it is).
static uint8_t* TplzPack(const uint8_t* blob, uint64_t n, uint64_t* outLen)
{
    uint8_t* z = (uint8_t*)malloc((size_t)(TPLZ_FRAME + TplzBound(n)));
    if (!z) return nullptr;
    const uint64_t h = TplzFnv(blob, n);
    memcpy(z, "TPTG", 4);
    memcpy(z + 4, &TPLZ_WIRE_PACKED, 4);
    memcpy(z + 8, &n, 8);
    memcpy(z + 16, &h, 8);
    const uint64_t zn = TplzCompress(blob, n, z + TPLZ_FRAME);
    if (TPLZ_FRAME + zn >= n) { free(z); return nullptr; }
    *outLen = TPLZ_FRAME + zn;
    return z;
}

static bool TplzIsPacked(const uint8_t* z, uint64_t n)
{
    uint32_t v = 0;
    if (n < TPLZ_FRAME || memcmp(z, "TPTG", 4) != 0) return false;
    memcpy(&v, z + 4, 4);
    return v == TPLZ_WIRE_PACKED;
}

// The blob out of a frame (malloc'd), or nullptr and why.
static uint8_t* TplzUnpack(const uint8_t* z, uint64_t n, uint64_t maxRaw, uint64_t* outLen, const char** why)
{
    uint64_t raw = 0, h = 0;
    if (!TplzIsPacked(z, n)) { *why = "not a packed frame"; return nullptr; }
    memcpy(&raw, z + 8, 8);
    memcpy(&h, z + 16, 8);
    if (raw == 0 || raw > maxRaw) { *why = "its size is out of range"; return nullptr; }
    uint8_t* out = (uint8_t*)malloc((size_t)raw);
    if (!out) { *why = "out of memory"; return nullptr; }
    if (!TplzDecompress(z + TPLZ_FRAME, n - TPLZ_FRAME, out, raw)) { free(out); *why = "the stream is damaged"; return nullptr; }
    if (TplzFnv(out, raw) != h) { free(out); *why = "its checksum does not match"; return nullptr; }
    *outLen = raw;
    return out;
}
