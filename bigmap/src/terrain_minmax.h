// Steam 35924 terrain tile publication (CTerrain height update, RVA 0x33cd10).
// Two bit-identical replacements on the save-load publication path; see
// docs/terrain-minmax.md for the disassembly, proofs and tests.
//
//  1. The inlined `anonymous-namespace'::CalcMinMaxHeight scan (0x33cec1..
//     0x33cf06, 69 bytes). Stock walks the tile's uint16 heights with two
//     compares per value. The patch calls an SSE2 unsigned min/max and hands
//     back exactly the stock register contract (r10w = min, cx = max, r11
//     preserved, xmm2 = [r14+0x34]); the float conversion, scale, assert and
//     store that follow stay stock code.
//  2. The uint16 block copy 0x30a540 (the pdata chunk 0x30a55c is its body).
//     Stock copies element by element; for non-overlapping source/destination
//     spans a per-row memcpy writes the same values in the same row order.
//     Anything else goes to the original.
// Included after H and g_gog. Steam only; refuses on GOG or any byte mismatch.
#pragma once
#include <emmintrin.h>

static bool g_terrainMinMaxFast = false;

// ---- 1. CalcMinMaxHeight scan --------------------------------------------
// Stock count: begin > end ? 0 : (end - begin + 1) >> 1, 64-bit unsigned.
// min and max both start at begin[0], which stock reads unconditionally.
// Stock's "if (v < min) min = v; else if (v > max) max = v" keeps min <= max,
// so its result is the true unsigned minimum and maximum of those values.
// Reads exactly the stock byte range [begin, begin + 2*count), no more.
static uint32_t __fastcall TerrainMinMaxScan(const uint16_t* begin, const uint16_t* end) {
    const uintptr_t b = uintptr_t(begin), e = uintptr_t(end);
    const size_t n = b > e ? 0 : size_t((e - b + 1) >> 1);
    unsigned lo = begin[0], hi = begin[0];
    size_t i = 0;
    if (n >= 32) {
        // SSE2 has only signed 16-bit min/max: bias by 0x8000 to order unsigned.
        const __m128i bias = _mm_set1_epi16(short(0x8000));
        __m128i vmin = _mm_set1_epi16(short(lo ^ 0x8000)), vmax = vmin;
        const auto load = [&](size_t k) {
            return _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(begin + k)), bias);
        };
        for (; i + 32 <= n; i += 32) {
            const __m128i a = load(i), c = load(i + 8), d = load(i + 16), f = load(i + 24);
            vmin = _mm_min_epi16(vmin, _mm_min_epi16(_mm_min_epi16(a, c), _mm_min_epi16(d, f)));
            vmax = _mm_max_epi16(vmax, _mm_max_epi16(_mm_max_epi16(a, c), _mm_max_epi16(d, f)));
        }
        for (; i + 8 <= n; i += 8) {
            const __m128i a = load(i);
            vmin = _mm_min_epi16(vmin, a);
            vmax = _mm_max_epi16(vmax, a);
        }
        uint16_t mins[8], maxs[8];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(mins), _mm_xor_si128(vmin, bias));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(maxs), _mm_xor_si128(vmax, bias));
        for (int k = 0; k < 8; ++k) {
            if (mins[k] < lo) lo = mins[k];
            if (maxs[k] > hi) hi = maxs[k];
        }
    }
    for (; i < n; ++i) {
        const unsigned v = begin[i];
        if (v < lo) lo = v; else if (v > hi) hi = v;
    }
    return lo | (hi << 16);
}

static const uintptr_t RVA_MINMAX_SCAN = 0x33cec1;   // movzx r10d, word ptr [rdx]
static const uintptr_t RVA_MINMAX_SCAN_END = 0x33cf06; // movzx eax, r10w (stock)
enum { MINMAX_SCAN_SIZE = 69 };
// The whole per-tile loop (0x33ce80..0x33cf4d): its head reloads rdx/rsi/r8/r9
// before use and its tail reads r10w, cx, xmm2 and r11. Pinning these bytes
// pins the register contract the patch relies on.
static const uint8_t kTerrainMinMaxLoopBytes[205] = {
    0x4d,0x8b,0x46,0x18,0x41,0x8b,0x13,0x41,0x2b,0x10,0x41,0x8b,0x43,0x04,0x41,0x2b,0x40,0x04,0x41,0x8b,
    0x48,0x08,0x0f,0xaf,0xc8,0x03,0xca,0x48,0x63,0xc1,0x48,0x8d,0x1c,0x80,0x49,0x8b,0x78,0x10,0x48,0x8b,
    0x44,0xdf,0x08,0xf3,0x41,0x0f,0x10,0x56,0x34,0x48,0x8b,0x70,0x08,0x48,0x8b,0x10,0x48,0x3b,0xd6,0x0f,
    0x84,0x41,0x01,0x00,0x00,0x44,0x0f,0xb7,0x12,0x41,0x0f,0xb7,0xca,0x4d,0x8b,0xc4,0x4c,0x8b,0xce,0x4c,
    0x2b,0xca,0x49,0xff,0xc1,0x49,0xd1,0xe9,0x48,0x3b,0xd6,0x4d,0x0f,0x47,0xcc,0x4d,0x85,0xc9,0x74,0x22,
    0x0f,0xb7,0x02,0x66,0x41,0x3b,0xc2,0x73,0x06,0x44,0x0f,0xb7,0xd0,0xeb,0x07,0x66,0x3b,0xc1,0x66,0x0f,
    0x47,0xc8,0x48,0x83,0xc2,0x02,0x49,0xff,0xc0,0x4d,0x3b,0xc1,0x75,0xde,0x41,0x0f,0xb7,0xc2,0x66,0x0f,
    0x6e,0xc0,0x0f,0x5b,0xc0,0xf3,0x0f,0x59,0xc2,0x0f,0xb7,0xc1,0x66,0x0f,0x6e,0xc8,0x0f,0x5b,0xc9,0xf3,
    0x0f,0x59,0xca,0x0f,0x2f,0xc8,0x0f,0x82,0xf7,0x00,0x00,0x00,0xf3,0x0f,0x11,0x44,0xdf,0x18,0xf3,0x0f,
    0x11,0x4c,0xdf,0x1c,0xff,0x44,0xdf,0x20,0x49,0x83,0xc3,0x08,0x4d,0x3b,0xdf,0x0f,0x85,0x37,0xff,0xff,
    0xff,0x4c,0x8b,0x5d,0x98
};
// Epilogue: rsi (the patch's scratch copy of r11) is restored from the frame.
static const uint8_t kTerrainMinMaxEpilogueBytes[28] = {
    0x4c,0x8d,0x9c,0x24,0x50,0x01,0x00,0x00,0x49,0x8b,0x5b,0x40,0x49,0x8b,0x73,0x48,0x49,0x8b,0xe3,0x41,
    0x5f,0x41,0x5e,0x41,0x5c,0x5f,0x5d,0xc3
};

// In: rdx = begin, rsi = end, r11 = tile iterator. The frame is fully set up
// (rsp % 16 == 0, [rsp..rsp+0x20) is the outgoing shadow area), so a plain call
// keeps the game's unwind state valid. rsi is dead until 0x33ceb1 reloads it
// and is callee-saved, so it carries r11 across the call. rax, rdx, r8, r9,
// xmm0/xmm1 are dead at 0x33cf06; xmm2 is reloaded from the same source stock
// loaded it from (0x33ceab).
static void BuildTerrainMinMaxPatch(uint8_t* p, uintptr_t scan) {
    static const uint8_t head[] = {
        0x48,0x89,0xd1,             // mov rcx, rdx
        0x48,0x89,0xf2,             // mov rdx, rsi
        0x4c,0x89,0xde,             // mov rsi, r11
        0x48,0xb8                   // mov rax, imm64
    };
    static const uint8_t tail[] = {
        0xff,0xd0,                  // call rax
        0x49,0x89,0xf3,             // mov r11, rsi
        0xf3,0x41,0x0f,0x10,0x56,0x34, // movss xmm2, dword ptr [r14+0x34]
        0x44,0x0f,0xb7,0xd0,        // movzx r10d, ax
        0xc1,0xe8,0x10,             // shr eax, 16
        0x89,0xc1,                  // mov ecx, eax
        0xeb,0x1c                   // jmp 0x33cf06
    };
    memset(p, 0xcc, MINMAX_SCAN_SIZE);
    memcpy(p, head, sizeof head);
    memcpy(p + sizeof head, &scan, 8);
    memcpy(p + sizeof head + 8, tail, sizeof tail);
}

// ---- 2. uint16 block copy (0x30a540) ---------------------------------------
// void (src, dst, srcStride, dstStride, srcX, srcY, w, h, dstX, dstY):
//   for r < h: for i < w:
//     dst[dstY*dstStride + r*dstStride + dstX + i] = src[srcY*srcStride + r*srcStride + srcX + i]
// The Y products are 32-bit imul (wrapping) then sign-extended; everything
// else is 64-bit. Callers: 3c40c0 (publication into the tile cache),
// 3ac330 GetHeightmap, 3c4620 BaseGetHeightmapRefined, 3c4a20 GetBlock,
// 316590, 32ea60.
using TerrainBlockCopyFn = void (__fastcall*)(const uint16_t*, uint16_t*, int, int, int, int, int, int, int, int);
static TerrainBlockCopyFn g_originalTerrainBlockCopy = nullptr;
// Set by terrain_serve.h: true when `dst` lies in a tile whose content came
// from the terrain sidecar, so the load's publication into it is skipped
// (the sidecar IS the published result). Null when the sidecar is off.
static bool (*g_terrainServedCheck)(const void* dst) = nullptr;
static volatile LONG64 g_terrainServedCopiesSkipped = 0;

static void __fastcall TerrainBlockCopyDetour(const uint16_t* src, uint16_t* dst, int srcStride,
    int dstStride, int srcX, int srcY, int w, int h, int dstX, int dstY)
{
    // Stock touches no memory for h <= 0 (skips) or w <= 0 (empty rows).
    if (h <= 0 || w <= 0) return;
    if (g_terrainServedCheck && g_terrainServedCheck(dst)) { InterlockedIncrement64(&g_terrainServedCopiesSkipped); return; }
    // Bounds that keep every offset below exact in int64 (|offset| < 2^53).
    if (h > (1 << 20) || w > (1 << 20)) {
        g_originalTerrainBlockCopy(src, dst, srcStride, dstStride, srcX, srcY, w, h, dstX, dstY);
        return;
    }
    const int64_t s0 = int32_t(uint32_t(srcStride) * uint32_t(srcY)) + int64_t(srcX);
    const int64_t d0 = int32_t(uint32_t(dstStride) * uint32_t(dstY)) + int64_t(dstX);
    const int64_t sLast = s0 + int64_t(h - 1) * srcStride, dLast = d0 + int64_t(h - 1) * dstStride;
    const int64_t bytes = int64_t(w) * 2;
    const int64_t srcBase = int64_t(uintptr_t(src)), dstBase = int64_t(uintptr_t(dst));
    const int64_t srcLo = srcBase + 2 * (s0 < sLast ? s0 : sLast);
    const int64_t srcHi = srcBase + 2 * (s0 < sLast ? sLast : s0) + bytes;
    const int64_t dstLo = dstBase + 2 * (d0 < dLast ? d0 : dLast);
    const int64_t dstHi = dstBase + 2 * (d0 < dLast ? dLast : d0) + bytes;
    // Any shared byte (including odd alignments or row self-overlap with the
    // source) could make stock's forward element order observable: keep stock.
    if (srcLo < dstHi && dstLo < srcHi) {
        g_originalTerrainBlockCopy(src, dst, srcStride, dstStride, srcX, srcY, w, h, dstX, dstY);
        return;
    }
    // Disjoint spans: every read sees the initial source, so each row writes
    // the same values; rows stay in stock order (destination rows may overlap).
    for (int64_t r = 0; r < h; ++r) {
        memcpy(reinterpret_cast<void*>(uintptr_t(dstBase) + uintptr_t(2 * (d0 + r * dstStride))),
               reinterpret_cast<const void*>(uintptr_t(srcBase) + uintptr_t(2 * (s0 + r * srcStride))),
               size_t(bytes));
    }
}
static const uint8_t kTerrainBlockCopyBytes[208] = {
    0x40,0x53,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x10,0x8b,0x5c,0x24,0x68,0x4c,0x8b,0xf2,0x4c,0x8b,0xf9,
    0x85,0xdb,0x0f,0x8e,0xaa,0x00,0x00,0x00,0x48,0x89,0x6c,0x24,0x30,0x41,0x8b,0xc0,0x0f,0xaf,0x44,0x24,
    0x58,0x48,0x89,0x74,0x24,0x38,0x48,0x89,0x7c,0x24,0x40,0x48,0x63,0x7c,0x24,0x60,0x4c,0x89,0x64,0x24,
    0x08,0x4c,0x63,0x64,0x24,0x70,0x4c,0x63,0xd8,0x41,0x8b,0xc1,0x0f,0xaf,0x44,0x24,0x78,0x4c,0x89,0x2c,
    0x24,0x4c,0x63,0x6c,0x24,0x50,0x49,0x63,0xf0,0x49,0x63,0xe9,0x4c,0x63,0xd0,0x90,0x48,0x85,0xff,0x7e,
    0x3d,0x4d,0x8b,0xc3,0x4b,0x8d,0x04,0x22,0x4d,0x2b,0xc2,0x49,0x8d,0x04,0x46,0x4d,0x2b,0xc4,0x48,0x8b,
    0xd7,0x4d,0x03,0xc5,0x4f,0x8d,0x0c,0x00,0x4d,0x2b,0xce,0x4d,0x03,0xcf,0x66,0x66,0x0f,0x1f,0x84,0x00,
    0x00,0x00,0x00,0x00,0x41,0x0f,0xb7,0x0c,0x01,0x66,0x89,0x08,0x48,0x8d,0x40,0x02,0x48,0x83,0xea,0x01,
    0x75,0xee,0x4c,0x03,0xd5,0x4c,0x03,0xde,0x48,0x83,0xeb,0x01,0x75,0xb2,0x4c,0x8b,0x2c,0x24,0x4c,0x8b,
    0x64,0x24,0x08,0x48,0x8b,0x7c,0x24,0x40,0x48,0x8b,0x74,0x24,0x38,0x48,0x8b,0x6c,0x24,0x30,0x48,0x83,
    0xc4,0x10,0x41,0x5f,0x41,0x5e,0x5b,0xc3
};
enum { TERRAIN_BLOCK_COPY_STEAL = 14 }; // push rbx/r14/r15; sub rsp,10h; mov ebx,[rsp+68h]

static bool InstallTerrainMinMaxFast() {
    if (!g_terrainMinMaxFast || g_gog) return false;
    // Preflight every site before touching anything.
    if (!H->verifyBytes(0x33ce80, kTerrainMinMaxLoopBytes, sizeof kTerrainMinMaxLoopBytes) ||
        !H->verifyBytes(0x33cfe6, kTerrainMinMaxEpilogueBytes, sizeof kTerrainMinMaxEpilogueBytes) ||
        !H->verifyBytes(0x30a540, kTerrainBlockCopyBytes, sizeof kTerrainBlockCopyBytes)) {
        H->log("terrain minmax fast: Steam 35924 byte mismatch; OFF"); return false;
    }
    // Each half is independently bit-identical, so a partial install is safe.
    uint8_t patch[MINMAX_SCAN_SIZE];
    BuildTerrainMinMaxPatch(patch, uintptr_t(&TerrainMinMaxScan));
    const bool scan = H->patchBytes(RVA_MINMAX_SCAN, patch, sizeof patch) != 0;
    const bool copy = H->installHook(H->moduleBase() + 0x30a540, reinterpret_cast<void*>(&TerrainBlockCopyDetour),
        TERRAIN_BLOCK_COPY_STEAL, reinterpret_cast<void**>(&g_originalTerrainBlockCopy)) != 0;
    H->log("terrain minmax fast: tile min/max SSE2 scan %s, height block row copy %s; results bit-identical",
           scan ? "enabled" : "FAILED", copy ? "enabled" : "FAILED");
    return scan || copy;
}

extern "C" __declspec(dllexport)
uint32_t BigmapTestTerrainMinMaxScan(const uint16_t* begin, const uint16_t* end) {
    return TerrainMinMaxScan(begin, end);
}
extern "C" __declspec(dllexport)
uintptr_t BigmapTestTerrainMinMaxScanAddress() { return uintptr_t(&TerrainMinMaxScan); }
extern "C" __declspec(dllexport)
void BigmapTestTerrainMinMaxPatch(uint8_t* out, uintptr_t scan) { BuildTerrainMinMaxPatch(out, scan); }
extern "C" __declspec(dllexport)
void BigmapTestTerrainBlockCopy(TerrainBlockCopyFn original, const uint16_t* src, uint16_t* dst,
    int srcStride, int dstStride, int srcX, int srcY, int w, int h, int dstX, int dstY) {
    const auto old = g_originalTerrainBlockCopy;
    g_originalTerrainBlockCopy = original;
    TerrainBlockCopyDetour(src, dst, srcStride, dstStride, srcX, srcY, w, h, dstX, dstY);
    g_originalTerrainBlockCopy = old;
}
// The benchmark calls the detour itself, with the stock ten-argument shape.
extern "C" __declspec(dllexport)
uintptr_t BigmapTestTerrainBlockCopyAddress() { return uintptr_t(&TerrainBlockCopyDetour); }
extern "C" __declspec(dllexport)
void BigmapTestTerrainBlockCopySetOriginal(TerrainBlockCopyFn fn) { g_originalTerrainBlockCopy = fn; }
extern "C" __declspec(dllexport)
int BigmapTestInstallTerrainMinMax(const Tpf2mpHost* host, int gog, int enabled) {
    const auto oldHost = H; const bool oldGog = g_gog, oldEnabled = g_terrainMinMaxFast;
    const auto oldOriginal = g_originalTerrainBlockCopy;
    H = host; g_gog = gog != 0; g_terrainMinMaxFast = enabled != 0;
    const bool result = InstallTerrainMinMaxFast();
    H = oldHost; g_gog = oldGog; g_terrainMinMaxFast = oldEnabled; g_originalTerrainBlockCopy = oldOriginal;
    return result;
}
