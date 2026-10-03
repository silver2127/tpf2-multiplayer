// Steam 35924 terrain_alignment_util::CalculateHeightMod at RVA 0x3b3470
// (game\terrain\terrain_alignment_list_util.cpp). One call per height-mod block
// from the ecs::TerrainAlignmentSystem::UpdateSubterrains thread-pool workers
// (0xaac460) and from 0x2146540: it re-applies the construction/street/track
// alignment triangles to a block of N = size.x * size.y height samples while the
// height cache is rebuilt.
//
// Bit-identical replacement (docs/terrain-alignment-speed.md). Stock work per call:
//  * three PredHeightModRasterizable targets, each holding two std::vector<uint16>
//    of N words. 0x3b0190 (twice) and 0x3af850 (six times) allocate six heap
//    blocks and fill them ONE WORD PER ITERATION with 0xffff, 0, 0xffff, 0, 0, 0;
//  * every alignment triangle rasterised into its target (0x2375420 / 0x23754b0
//    with the predicates 0x3b7440 and 0x3b6ee0);
//  * a scalar pass over all N samples that blends the three targets into the
//    result wherever any of the three weights is non-zero.
// What changed:
//  * the six vectors come from a pooled buffer reused across calls and reset with
//    two memsets (same N words, same 0xffff / 0 values) -- no allocation, no
//    per-call first touch of fresh pages, no word-at-a-time fill;
//  * triangles are still rasterised by the ORIGINAL code: the targets are laid
//    out byte for byte as 0x3b0190 lays them out (vtable, sizes, scale, 1/scale,
//    offset, the two vectors) and the predicates read nothing else;
//  * the blend skips all-zero-weight samples eight at a time and evaluates the
//    rest four lanes wide with SSE2 -- the same IEEE single-precision operations
//    on the same operands in the same grouping, comparisons with the same
//    ordered/unordered outcome (cmpps matches comiss/ucomiss + the stock branch),
//    and floorf + cvttss2si reproduced exactly. No FMA, no reassociation.
// The stock size assert, blocks with a side < 2 or more than 1<<20 samples, an
// alignment type the stock code cannot index, malformed vectors and a failed
// buffer allocation all run the original.
#pragma once
#include <emmintrin.h>
#include <cstddef>
#include <malloc.h>
#include <string.h>

struct AlignU16Vector { uint16_t* first; uint16_t* last; uint16_t* end; };
struct AlignPointerVector { const uint8_t* const* first; const uint8_t* const* last;
                            const uint8_t* const* end; };
using CalculateHeightModFn = void (__fastcall*)(const float*, const int32_t*, float, float,
    const AlignPointerVector*, AlignU16Vector*);
using AlignRasterInitFn = void* (__fastcall*)(void*, void*, const float*, const float*,
    const int32_t*, uint8_t);
using AlignRasterTriangleFn = uint8_t (__fastcall*)(void*, uint64_t, uint64_t, uint64_t);
using AlignAssertFn = void (__fastcall*)(const char*, const char*, int, const char*);

// terrain::TerrainAlignment, only the fields 0x3b3470 reads.
static const size_t kAlignTriangles = 0x00;    // vector<CVec3f>: 3 vertices (36 bytes) per triangle
static const size_t kAlignWeights   = 0x18;    // vector<CVec3f>: one weight per vertex, may be empty
static const size_t kAlignType      = 0x30;    // 0, 1 or 2: which rasterisation target
static const int64_t kAlignMaxSamples = 1 << 20;

static const uintptr_t kAlignFunc            = 0x3b3470;
static const uintptr_t kAlignRasterInit      = 0x2375420;
static const uintptr_t kAlignRasterTriangle  = 0x23754b0;
static const uintptr_t kAlignAssert          = 0x221adf0;
static const uintptr_t kAlignVtableLessEqual = 0x2fb64c0;   // targets 0 and 1, heights start 0xffff
static const uintptr_t kAlignVtableGreaterEqual = 0x2fb64d8;// target 2, heights start 0
static const uintptr_t kAlignAssertExpr      = 0x2fb6658;
static const uintptr_t kAlignAssertFile      = 0x2fb4fa0;
static const uintptr_t kAlignAssertFunction  = 0x2fb64f0;

// PredHeightModRasterizable<...>, exactly as 0x3b0190 builds it.
struct AlignTarget {
    uintptr_t vtable;                   // +0x00
    unsigned char triangle[0x30];       // +0x08 three vertices then three weights, zeroed by the ctor
    int32_t sizeX, sizeY;               // +0x38
    float scale, invScale, offset;      // +0x40
    uint32_t padding;                   // +0x4c untouched by the ctor and unread by the predicates
    AlignU16Vector heights;             // +0x50
    AlignU16Vector weights;             // +0x68
};
static_assert(offsetof(AlignTarget, sizeX) == 0x38, "target layout");
static_assert(offsetof(AlignTarget, scale) == 0x40, "target layout");
static_assert(offsetof(AlignTarget, heights) == 0x50, "target layout");
static_assert(offsetof(AlignTarget, weights) == 0x68, "target layout");
static_assert(sizeof(AlignTarget) == 0x80, "target layout");

static CalculateHeightModFn g_originalCalculateHeightMod = nullptr;
static uintptr_t g_terrainAlignBase = 0;
static bool g_terrainAlignFast = false;

// ---------------------------------------------------------------- scratch pool
// One buffer of 6*N words per concurrent call: heightA, heightB, weightA,
// weightB, heightC, weightC. Pooled because the stock code's six allocations and
// their word-at-a-time fills are ~5% of a big save load.
struct AlignScratch { AlignScratch* next; size_t samples; uint16_t* words; };
static SRWLOCK g_alignScratchLock = SRWLOCK_INIT;
static AlignScratch* g_alignScratchFree = nullptr;

static void ReleaseAlignScratch(AlignScratch* scratch) {
    AcquireSRWLockExclusive(&g_alignScratchLock);
    scratch->next = g_alignScratchFree;
    g_alignScratchFree = scratch;
    ReleaseSRWLockExclusive(&g_alignScratchLock);
}

static AlignScratch* AcquireAlignScratch(size_t samples) {
    AcquireSRWLockExclusive(&g_alignScratchLock);
    AlignScratch* scratch = g_alignScratchFree;
    if (scratch) g_alignScratchFree = scratch->next;
    ReleaseSRWLockExclusive(&g_alignScratchLock);
    if (!scratch) {
        scratch = static_cast<AlignScratch*>(malloc(sizeof(AlignScratch)));
        if (!scratch) return nullptr;
        scratch->next = nullptr; scratch->samples = 0; scratch->words = nullptr;
    }
    if (scratch->samples < samples) {
        size_t want = samples < 66049 ? 66049 : samples;      // a 1 m tile block is 257x257
        want = (want + 0xfff) & ~size_t(0xfff);
        _aligned_free(scratch->words);
        scratch->words = static_cast<uint16_t*>(_aligned_malloc(6 * want * sizeof(uint16_t), 64));
        scratch->samples = scratch->words ? want : 0;
        if (!scratch->words) { ReleaseAlignScratch(scratch); return nullptr; }
    }
    return scratch;
}

// ---------------------------------------------------------------- the blend
struct AlignBlendInput {
    const uint16_t* result;
    const uint16_t* heightA; const uint16_t* heightB; const uint16_t* heightC;
    const uint16_t* weightA; const uint16_t* weightB; const uint16_t* weightC;
};
struct AlignBlendConstants { __m128 scale, offset, invScale, zero, one, half, full; };

static inline __m128 AlignWiden(__m128i words, int upper) {
    const __m128i zero = _mm_setzero_si128();
    return _mm_cvtepi32_ps(upper ? _mm_unpackhi_epi16(words, zero) : _mm_unpacklo_epi16(words, zero));
}
static inline __m128 AlignSelect(__m128 mask, __m128 taken, __m128 other) {
    return _mm_or_ps(_mm_and_ps(mask, taken), _mm_andnot_ps(mask, other));
}

// Eight samples. Returns a bitmask of lanes to store; *failLane, if set, is the
// lane where the stock "totalW > .0f" assert fires (lanes above it are dropped).
static int AlignBlendEight(const AlignBlendInput& in, const AlignBlendConstants& k,
                           int32_t* values, int* failLane)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i wordsA = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.weightA));
    const __m128i wordsB = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.weightB));
    const __m128i wordsC = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.weightC));
    // Stock skips a sample when (int)wA + (int)wB + (int)wC == 0, i.e. all three zero.
    const int nonzero = ~_mm_movemask_epi8(
        _mm_cmpeq_epi16(_mm_or_si128(_mm_or_si128(wordsA, wordsB), wordsC), zero)) & 0xffff;
    if (!nonzero) return 0;
    const __m128i rWords  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.result));
    const __m128i haWords = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.heightA));
    const __m128i hbWords = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.heightB));
    const __m128i hcWords = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in.heightC));
    int written = 0;
    for (int upper = 0; upper < 2; ++upper) {
        int lanes = 0;
        for (int lane = 0; lane < 4; ++lane)
            if ((nonzero >> (2 * (4 * upper + lane))) & 3) lanes |= 1 << lane;
        if (!lanes) continue;
        // height = (float)word * scale + offset, as the stock cvtdq2ps/mulss/addss.
        const __m128 heightR = _mm_add_ps(_mm_mul_ps(AlignWiden(rWords, upper), k.scale), k.offset);
        const __m128 heightA = _mm_add_ps(_mm_mul_ps(AlignWiden(haWords, upper), k.scale), k.offset);
        const __m128 heightB = _mm_add_ps(_mm_mul_ps(AlignWiden(hbWords, upper), k.scale), k.offset);
        const __m128 heightC = _mm_add_ps(_mm_mul_ps(AlignWiden(hcWords, upper), k.scale), k.offset);
        const __m128 fractionA = _mm_div_ps(AlignWiden(wordsA, upper), k.full);
        __m128 fractionB = _mm_div_ps(AlignWiden(wordsB, upper), k.full);
        __m128 fractionC = _mm_div_ps(AlignWiden(wordsC, upper), k.full);
        // lower = min(fC > 0 ? max(result, C) : result, B); upper = max(fB > 0 ? min(result, B) : result, C)
        __m128 lower = AlignSelect(_mm_and_ps(_mm_cmpgt_ps(fractionC, k.zero),
                                              _mm_cmpgt_ps(heightC, heightR)), heightC, heightR);
        lower = AlignSelect(_mm_cmpgt_ps(lower, heightB), heightB, lower);
        __m128 higher = AlignSelect(_mm_and_ps(_mm_cmpgt_ps(fractionB, k.zero),
                                               _mm_cmpgt_ps(heightR, heightB)), heightB, heightR);
        higher = AlignSelect(_mm_cmpgt_ps(heightC, higher), heightC, higher);
        fractionB = _mm_and_ps(_mm_cmpeq_ps(lower, heightB), fractionB);
        fractionC = _mm_and_ps(_mm_cmpeq_ps(higher, heightC), fractionC);
        const __m128 sum = _mm_add_ps(_mm_add_ps(fractionB, fractionA), fractionC);
        int active = lanes & ~_mm_movemask_ps(_mm_cmpeq_ps(sum, k.zero));
        if (!active) continue;
        // A weight of exactly 1 in two of the three drops the later one.
        const __m128 oneA = _mm_cmpeq_ps(fractionA, k.one);
        const __m128 oneB = _mm_cmpeq_ps(fractionB, k.one);
        const __m128 oneC = _mm_cmpeq_ps(fractionC, k.one);
        fractionB = _mm_andnot_ps(_mm_and_ps(oneA, oneB), fractionB);
        fractionC = _mm_andnot_ps(_mm_and_ps(oneC, _mm_or_ps(oneA, oneB)), fractionC);
        const __m128 restA = _mm_sub_ps(k.one, fractionA);
        const __m128 restB = _mm_sub_ps(k.one, fractionB);
        const __m128 restC = _mm_sub_ps(k.one, fractionC);
        const __m128 weightA = _mm_mul_ps(_mm_mul_ps(restB, fractionA), restC);
        const __m128 weightB = _mm_mul_ps(_mm_mul_ps(restA, fractionB), restC);
        const __m128 weightC = _mm_mul_ps(_mm_mul_ps(restA, restB), fractionC);
        const __m128 total = _mm_add_ps(_mm_add_ps(weightB, weightA), weightC);
        const __m128 positive = _mm_cmpgt_ps(total, k.zero);
        const int bad = active & ~_mm_movemask_ps(positive);
        // Inactive lanes divide by 1: no stock operation is added, and an
        // unmasked divide-by-zero cannot fire where stock would not divide.
        const __m128 divisor = AlignSelect(positive, total, k.one);
        __m128 value = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lower, weightB), _mm_mul_ps(weightA, heightA)),
                                  _mm_mul_ps(higher, weightC));
        value = _mm_div_ps(value, divisor);
        value = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(value, k.offset), k.invScale), k.half);
        // floorf then cvttss2si: truncation, minus one where truncation rounded
        // up, and the 0x80000000 "integer indefinite" of NaN and out-of-range
        // values kept as is (stock floors them to themselves).
        __m128i truncated = _mm_cvttps_epi32(value);
        const __m128i indefinite = _mm_cmpeq_epi32(truncated, _mm_set1_epi32(INT32_MIN));
        const __m128i rounded = _mm_castps_si128(_mm_cmplt_ps(value, _mm_cvtepi32_ps(truncated)));
        truncated = _mm_add_epi32(truncated, _mm_andnot_si128(indefinite, rounded));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(values + 4 * upper), truncated);
        if (bad) {
            unsigned long lane = 0;
            _BitScanForward(&lane, static_cast<unsigned long>(bad));
            written |= (active & ((1 << lane) - 1)) << (4 * upper);
            *failLane = 4 * upper + static_cast<int>(lane);
            return written;
        }
        written |= active << (4 * upper);
    }
    return written;
}

// Returns the sample index where the stock assert fires, or -1.
static int64_t AlignBlend(uint16_t* out, const uint16_t* words, size_t samples,
                          const AlignBlendConstants& k)
{
    AlignBlendInput in;
    const uint16_t* heightA = words;
    const uint16_t* heightB = words + samples;
    const uint16_t* weightA = words + 2 * samples;
    const uint16_t* weightB = words + 3 * samples;
    const uint16_t* heightC = words + 4 * samples;
    const uint16_t* weightC = words + 5 * samples;
    uint16_t tail[7][8];
    for (size_t i = 0; i < samples; i += 8) {
        const size_t count = samples - i < 8 ? samples - i : 8;
        const uint16_t* source[7] = {out + i, heightA + i, heightB + i, heightC + i,
                                     weightA + i, weightB + i, weightC + i};
        if (count < 8) {
            for (int s = 0; s < 7; ++s) {
                memset(tail[s], 0, sizeof tail[s]);         // padding lanes: weight 0, never written
                memcpy(tail[s], source[s], count * sizeof(uint16_t));
                source[s] = tail[s];
            }
        }
        in.result = source[0]; in.heightA = source[1]; in.heightB = source[2]; in.heightC = source[3];
        in.weightA = source[4]; in.weightB = source[5]; in.weightC = source[6];
        int32_t values[8];
        int failLane = -1;
        const int written = AlignBlendEight(in, k, values, &failLane);
        for (int lane = 0; lane < 8; ++lane)
            if ((written >> lane) & 1) out[i + lane] = static_cast<uint16_t>(values[lane]);
        if (failLane >= 0) return static_cast<int64_t>(i) + failLane;
    }
    return -1;
}

// ---------------------------------------------------------------- the call
static bool AlignListSupported(const AlignPointerVector* list) {
    const uintptr_t first = reinterpret_cast<uintptr_t>(list->first);
    const uintptr_t last = reinterpret_cast<uintptr_t>(list->last);
    if (last < first || (last - first) % sizeof(void*)) return false;
    for (const uint8_t* const* item = list->first; item != list->last; ++item) {
        const uint8_t* alignment = *item;
        uintptr_t begin = 0, end = 0;
        memcpy(&begin, alignment + kAlignTriangles, sizeof begin);
        memcpy(&end, alignment + kAlignTriangles + sizeof(void*), sizeof end);
        if (end < begin) return false;
        const uint64_t count = (end - begin) / 36;
        if (count > 0x7fffffff) return false;
        if (count) {                       // stock reads the type only when it rasterises
            int32_t type = 0;
            memcpy(&type, alignment + kAlignType, sizeof type);
            if (type < 0 || type > 2) return false;
        }
    }
    return true;
}

static void CalculateHeightModImpl(CalculateHeightModFn original, uintptr_t base, const float* box,
    const int32_t* size, float scale, float offset, const AlignPointerVector* alignments,
    AlignU16Vector* result)
{
    const int32_t sizeX = size[0], sizeY = size[1];
    const int64_t resultWords =
        (reinterpret_cast<intptr_t>(result->last) - reinterpret_cast<intptr_t>(result->first)) >> 1;
    if (int32_t(uint32_t(sizeX) * uint32_t(sizeY)) != int32_t(resultWords) ||   // the stock assert
        sizeX < 2 || sizeY < 2 || int64_t(sizeX) * int64_t(sizeY) > kAlignMaxSamples ||
        !base || !AlignListSupported(alignments)) {
        original(box, size, scale, offset, alignments, result);
        return;
    }
    const size_t samples = size_t(sizeX) * size_t(sizeY);
    AlignScratch* scratch = AcquireAlignScratch(samples);
    if (!scratch) {
        original(box, size, scale, offset, alignments, result);
        return;
    }
    uint16_t* words = scratch->words;
    memset(words, 0xff, 4 * samples);                       // heights of targets 0 and 1: 0xffff
    memset(words + 2 * samples, 0, 8 * samples);            // their weights, and target 2 entirely

    const float invScale = _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(1.0f), _mm_set_ss(scale)));
    AlignTarget targets[3] = {};
    for (int t = 0; t < 3; ++t) {
        targets[t].vtable = base + (t == 2 ? kAlignVtableGreaterEqual : kAlignVtableLessEqual);
        targets[t].sizeX = sizeX;
        targets[t].sizeY = sizeY;
        targets[t].scale = scale;
        targets[t].invScale = invScale;
        targets[t].offset = offset;
    }
    const size_t slot[3][2] = {{0, 2}, {1, 3}, {4, 5}};     // heights, weights
    for (int t = 0; t < 3; ++t) {
        uint16_t* height = words + slot[t][0] * samples;
        uint16_t* weight = words + slot[t][1] * samples;
        targets[t].heights.first = height;
        targets[t].heights.last = targets[t].heights.end = height + samples;
        targets[t].weights.first = weight;
        targets[t].weights.last = targets[t].weights.end = weight + samples;
    }

    __declspec(align(16)) unsigned char rasterizers[3][0x30] = {};
    const auto rasterInit = reinterpret_cast<AlignRasterInitFn>(base + kAlignRasterInit);
    const auto rasterTriangle = reinterpret_cast<AlignRasterTriangleFn>(base + kAlignRasterTriangle);
    for (int t = 0; t < 3; ++t) rasterInit(rasterizers[t], &targets[t], box, box + 2, size, 1);

    static const float kDefaultWeights[3] = {1.0f, 1.0f, 1.0f};   // the stock (1,1,1) scratch vector
    for (const uint8_t* const* item = alignments->first; item != alignments->last; ++item) {
        const uint8_t* alignment = *item;
        uintptr_t begin = 0, end = 0, weightBegin = 0, weightEnd = 0;
        memcpy(&begin, alignment + kAlignTriangles, sizeof begin);
        memcpy(&end, alignment + kAlignTriangles + sizeof(void*), sizeof end);
        memcpy(&weightBegin, alignment + kAlignWeights, sizeof weightBegin);
        memcpy(&weightEnd, alignment + kAlignWeights + sizeof(void*), sizeof weightEnd);
        const int32_t count = int32_t((end - begin) / 36);
        if (count <= 0) continue;
        int32_t type = 0;
        memcpy(&type, alignment + kAlignType, sizeof type);
        AlignTarget& target = targets[type];
        void* rasterizer = rasterizers[type];
        for (int32_t i = 0; i < count; ++i) {
            const uint8_t* vertex = reinterpret_cast<const uint8_t*>(begin) + size_t(i) * 36;
            const uint8_t* weight = weightBegin == weightEnd
                ? reinterpret_cast<const uint8_t*>(kDefaultWeights)
                : reinterpret_cast<const uint8_t*>(weightBegin) + size_t(i) * 12;
            memcpy(target.triangle + 0x00, vertex + 0x18, 12);   // +0x08: vertex 2
            memcpy(target.triangle + 0x0c, vertex + 0x0c, 12);   // +0x14: vertex 1
            memcpy(target.triangle + 0x18, vertex + 0x00, 12);   // +0x20: vertex 0
            memcpy(target.triangle + 0x24, weight + 8, 4);       // +0x2c: weight of vertex 2
            memcpy(target.triangle + 0x28, weight + 4, 4);       // +0x30: weight of vertex 1
            memcpy(target.triangle + 0x2c, weight + 0, 4);       // +0x34: weight of vertex 0
            uint64_t second = 0, first = 0, zeroth = 0;
            memcpy(&second, vertex + 0x18, 8);
            memcpy(&first, vertex + 0x0c, 8);
            memcpy(&zeroth, vertex + 0x00, 8);
            rasterTriangle(rasterizer, second, first, zeroth);
        }
    }

    AlignBlendConstants k;
    k.scale = _mm_set1_ps(scale);
    k.offset = _mm_set1_ps(offset);
    k.invScale = _mm_set1_ps(invScale);
    k.zero = _mm_setzero_ps();
    k.one = _mm_set1_ps(1.0f);
    k.half = _mm_set1_ps(0.5f);
    k.full = _mm_set1_ps(65535.0f);
    const int64_t failed = AlignBlend(result->first, words, samples, k);
    ReleaseAlignScratch(scratch);
    if (failed >= 0) {          // unreachable: see the exactness argument in the doc
        const auto fail = reinterpret_cast<AlignAssertFn>(base + kAlignAssert);
        fail(reinterpret_cast<const char*>(base + kAlignAssertExpr),
             reinterpret_cast<const char*>(base + kAlignAssertFile), 0x4d7,
             reinterpret_cast<const char*>(base + kAlignAssertFunction));
    }
}

static void __fastcall CalculateHeightModDetour(const float* box, const int32_t* size, float scale,
    float offset, const AlignPointerVector* alignments, AlignU16Vector* result)
{
    CalculateHeightModImpl(g_originalCalculateHeightMod, g_terrainAlignBase, box, size, scale,
                           offset, alignments, result);
}

// ---------------------------------------------------------------- installer
static const uint8_t kCalculateHeightModPrologue[21] = {
    0x48,0x8b,0xc4,                             // mov rax, rsp
    0x55, 0x56, 0x57,                           // push rbp; push rsi; push rdi
    0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57, // push r12..r15
    0x48,0x8d,0xa8,0xf8,0xfc,0xff,0xff          // lea rbp,[rax-0x308]
};
// Everything the replacement reproduces: the rest of the function body (which
// also pins its call targets, vtable and constant addresses), the constructor
// that fixes the target layout and the fill values, the resize whose fill this
// replaces, the three float constants and the assert message.
struct TerrainAlignRegion { uintptr_t rva; uint32_t size; const char* hex; };
static const TerrainAlignRegion kTerrainAlignRegions[] = {
    {0x3b3485,2132, // CalculateHeightMod body after the hook prologue
        "4881ecd003000048c745d8feffffff488958180f2970b80f2978a8440f294098440f294888440f299078ffffff440f299868ffffff440f29a058ffffff440f29a848ffffff44"
        "0f29b038ffffff440f29b828ffffff488b05d78ce1034833c448898520020000440f28e3440f28ea488bfa4c8bf14c8bbd300300004c8ba5380300004c8965a0498b4c240849"
        "2b0c2448d1f9448b42048b128bc2410fafc03bc10f858d070000bbffff000066895c2428f3440f116424200f28da488d8d90010000e845ccffff9066895c2428f3440f116424"
        "20410f28dd448b47048b17488d8d00010000e822ccffff9033f666897424308b4f048b17450f57c0897424480f57c0410f14c0f20f1145788bc6898580000000897424480f57"
        "c0410f14c0f20f11858400000089858c000000897424480f57c0410f14c0f20f1185900000008985980000004889b59c00000089b5a4000000488d05fb2ec002488945708995"
        "a8000000898dac000000f3440f11adb0000000f3440f10158db3b602410f28c2f3410f5ec5f30f11442460f30f1185b4000000f3440f11a5b80000000f57c0660f7f85c00000"
        "004889b5d00000004889b5d8000000660f7f85e00000000fafd14863d2488d85c00000004889442440488d44243048894424480f28442440660f7f45804c8d4580488d8dc000"
        "0000e8dac1ffff66897424388b85ac0000000faf85a80000004863d0488d85d80000004889442440488d44243848894424480f28442440660f7f45804c8d4580488d8dd80000"
        "00e895c1ffff90488d8590010000488945a8488d8500010000488945b0488d4570488945b8c64424280148897c24204d8d4e084d8bc6488d9590010000488d4de0e8251dfc01"
        "c64424280148897c24204d8d4e084d8bc6488d9500010000488d4d10e8041dfc01c64424280148897c24204d8d4e084d8bc6488d5570488d4d40e8e61cfc01488d45e0488945"
        "c0488d4510488945c8488d4540488945d00f57c0f30f7f4424688bce48894c2478498b57084889542450498b07488944245849bbabaaaaaaaaaaaa2a4c8b542468483bc20f84"
        "f601000048bf398ee3388ee3380e6666660f1f8400000000004c8b38498d5f184c8b43084c39030f858a000000498b4f08492b0f488bc748f7e94c8bca49d1f9498bc148c1e8"
        "3f4c03c8488b4c2470492bca498bc348f7e948d1fa488bc248c1e83f4803d04c3bca764dc74424400000803fc74424440000803fc74424480000803f488d4424684889458048"
        "8d442440488945880f284580660f7f45904c8d4590498bd1488d4c2468e81bc2ffff4c8b43084c8b5424684c8d6c24684c39034c0f45eb498b4f08492b0f488bc748f7e948d1"
        "fa488bc248c1e83f4803d04c63e285d20f8ee600000049634730488b7cc5a8488b4cc5c04c8bf6488bde488bf166660f1f840000000000498b0f498b4500f3410f101c06f341"
        "0f10540604f3410f104c0608f20f10440b18f20f1147088b440b20894710f20f10440b0cf20f1147148b440b1489471cf20f10040bf20f1147208b440b08894728f30f114f2c"
        "f30f115730f30f115f34f30f104c0b04f30f10540b10f30f105c0b1cf30f10040b0f14c166490f7ec1f30f104c0b0c0f14ca66490f7ec8f30f10540b180f14d366480f7ed248"
        "8bcee87e1bfc01488d5b244d8d760c4983ec010f854cffffff4c8b54246833f648bf398ee3388ee3380e488b4424584883c0084889442458483b44245049bbabaaaaaaaaaaaa"
        "2a0f8528feffff488b4c24784c8b65a04d85d2744e492bca498bc348f7e948d1fa488bc248c1e83f4803d0488d145248c1e202498bc24881fa00100000721c4883c2274d8b52"
        "f8492bc24883c0f84883f81f7607ff15057eb502cc498bcae8e8008402498b442408492b042448d1f84863f885c00f8e27020000f3440f10358456be02f3440f103d8bafb602"
        "0f1f00498b1c240fb70473660f6ed80f5bdbf3410f59ddf3410f58dcf30f115c2450488b85f80100000fb71470488b85680100000fb70c70488b85d8000000440fb70470448b"
        "c98d040a4103c00f84b6010000488b85e00100000fb70c70450f57dbf3440f2ad9f3450f59ddf3450f58dc488b85500100000fb70c700f57c0f30f2ac1f3410f59c5f3410f58"
        "c4f30f11442458488b85c00000000fb70c700f57c9f30f2ac9f3410f59cdf3410f58ccf30f114d90450f57c9f3440f2acaf3450f5ece0f57e4f3410f2ae1f3410f5ee60f57f6"
        "f3410f2af0f3410f5ef6410f2ff07616488d442450488d4d900f2fcb480f47c1f30f1010eb030f28d30f2fd076030f28d0410f2fe07615488d442450488d4c24580f2fd8480f"
        "47c1f30f10180f2fcb76030f28d90f2ed07a027404410f28e00f2ed97a027404410f28f00f28c4f3410f58c1f30f58c6410f2ec07a060f84b5000000450f2eca7a10750e410f"
        "2ee27a10750e410f28e0eb08410f2ee27a0e750c410f2ef27a067504410f28f0410f28caf30f5ccc410f28c2f30f5cc60f28f9f3410f59f9f30f59f8410f28eaf3410f5ce944"
        "0f28cdf3440f59ccf3440f59c8f30f59e9f30f59ee410f28c1f30f58c7f30f58c5410f2fc00f86cb000000f3410f59d1f3410f59fbf30f58d7f30f59ddf30f58d3f30f5ed0f3"
        "410f5cd4f30f59542460f3410f58d70f28c2e83d2c8402f30f2cc06689047348ffc6483bf70f8ceefdffff488d4d70e8d5c8ffff90488d8d00010000e8c8c8ffff90488d8d90"
        "010000e8bbc8ffff488b8d200200004833cce8ecfd83024c8d9c24d0030000498b5b50410f2873f0410f287be0450f2843d0450f284bc0450f2853b0450f285ba0450f286390"
        "450f286b80450f28b370ffffff450f28bb60ffffff498be3415f415e415d415c5f5e5dc34c8d0d5228c00241b8d7040000488d15f512c002488d0da629c002e83971e601904c"
        "8d0d3128c00241b879040000488d15d412c002488d0d5d29c002e81871e601cc"
    },
    {0x3b0190,317, // PredHeightModRasterizable constructor: the target layout and the 0xffff/0 fills
        "4c8bdc49894b08574883ec4049c743d8feffffff49895b1049897318488bf90f57d2c7442438000000000f57c00f14c2f20f1141088b442438894110c7442438000000000f57"
        "c00f14c2f20f1141148b44243889411cc7442438000000000f57c00f14c2f20f1141208b44243889412833f64889712c897134488d05b062c0024889018951384489413cf30f"
        "115940f30f100565e7b602f30f5ec3f30f114144f30f10442470f30f1141484883c1504889314889710848897110488d5f6848893348897308488973108b473c0faf47384863"
        "d049894be8498d4330498943f00f28442430660f7f4424304d8d43e8e8cdf5ffff66897424708b473c0faf47384863d048895c2430488d44247048894424380f28442430660f"
        "7f4424304c8d442430488bcbe897f5ffff90488bc7488b5c2458488b7424604883c4405fc3"
    },
    {0x3af850,504, // vector<unsigned short>::_Resize(n, value): the fill this replaces
        "48894c240856574154415641574883ec3048c7442428feffffff48895c2470498bf8488bf24c8bf9488b5108488b01488bda482bd848d1fb488b4910482bc848d1f9483bf10f"
        "865701000049b8ffffffffffffff7f493bf00f8794010000488bd148d1ea498bc0482bc2483bc87605488bc6eb0b488d040a483bc6480f42c648894424204c8d2400498bd449"
        "3bc0760c48c7c0ffffffff488bd0eb104881fa00100000723148c7c0ffffffff488d4a27483bca480f46c8e8744184024885c0740e4c8d70274983e6e0498946f8eb1cff15ab"
        "beb502cc4885d2740d488bcae84d4184024c8bf0eb034533f64c89742478498d0c5e4c8b4708488bd6482bd37411410fb700668901488d49024883ea0175ef498b7f084c8d44"
        "2468488d542468488d4c2468e877deceff492b3f4c8bc7498b17498bcee8d36d840290498b0f4885c97433498b5710482bd148d1fa4803d24881fa0010000072184883c2274c"
        "8b41f8492bc8488d41f84883f81f772d498bc8e8f64084024d8937498d0476498947084b8d043449894710488b5c24704883c430415f415e415c5f5ec3ff15dfbdb502cc483b"
        "f3762f498b4808482bf374100fb7016689024883c2024883ee0175f049895708488b5c24704883c430415f415e415c5f5ec37408488d047049894708488b5c24704883c43041"
        "5f415e415c5f5ec3e8a990ceffcc"
    },
    {0x2f99078,4, // 65535.0f
        "00ff7f47"
    },
    {0x2f1e98c,4, // 1.0f
        "0000803f"
    },
    {0x2f1e988,4, // 0.5f
        "0000003f"
    },
    {0x2fb6658,13, // "totalW > .0f"
        "746f74616c57203e202e306600"
    },
};

static bool VerifyTerrainAlignRegion(const TerrainAlignRegion& region) {
    static uint8_t bytes[2200];
    if (region.size > sizeof bytes || strlen(region.hex) != size_t(region.size) * 2) return false;
    for (uint32_t i = 0; i < region.size; ++i) {
        int value = 0;
        for (int n = 0; n < 2; ++n) {
            const char c = region.hex[2 * i + n];
            value = value * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -256);
        }
        if (value < 0) return false;
        bytes[i] = uint8_t(value);
    }
    return H->verifyBytes(region.rva, bytes, region.size) != 0;
}

static bool InstallTerrainAlignFast() {
    if (!g_terrainAlignFast || g_gog) return false;
    if (!H->verifyBytes(kAlignFunc, kCalculateHeightModPrologue, sizeof kCalculateHeightModPrologue)) {
        H->log("terrain align fast: prologue mismatch; OFF"); return false;
    }
    for (const auto& region : kTerrainAlignRegions) {
        if (!VerifyTerrainAlignRegion(region)) {
            H->log("terrain align fast: byte mismatch at RVA 0x%llx; OFF",
                   (unsigned long long)region.rva); return false;
        }
    }
    const uintptr_t base = H->moduleBase();
    // The two predicate vtables carry relocated pointers, so they are checked
    // against the running base, not against fixed bytes.
    const uintptr_t vtables[2][2] = {{base + 0x3b09a0, base + 0x3b7440},
                                     {base + 0x3b09a0, base + 0x3b6ee0}};
    const uintptr_t vtableRva[2] = {kAlignVtableLessEqual, kAlignVtableGreaterEqual};
    for (int i = 0; i < 2; ++i) {
        if (!H->verifyBytes(vtableRva[i], reinterpret_cast<const uint8_t*>(vtables[i]),
                            sizeof vtables[i])) {
            H->log("terrain align fast: predicate vtable mismatch at RVA 0x%llx; OFF",
                   (unsigned long long)vtableRva[i]); return false;
        }
    }
    void* original = nullptr;
    if (!H->installHook(base + kAlignFunc, (void*)&CalculateHeightModDetour,
                        sizeof kCalculateHeightModPrologue, &original)) {
        H->log("terrain align fast: hook failed; OFF"); return false;
    }
    g_originalCalculateHeightMod = reinterpret_cast<CalculateHeightModFn>(original);
    g_terrainAlignBase = base;
    H->log("terrain align fast: pooled alignment targets and SSE2 height-mod blend enabled "
           "(bit-identical; stock asserts and unusual blocks use the original)");
    return true;
}

extern "C" __declspec(dllexport)
void BigmapTestTerrainAlign(CalculateHeightModFn original, uintptr_t base, const float* box,
    const int32_t* size, float scale, float offset, const AlignPointerVector* alignments,
    AlignU16Vector* result) {
    CalculateHeightModImpl(original, base, box, size, scale, offset, alignments, result);
}
extern "C" __declspec(dllexport)
int BigmapTestInstallTerrainAlign(const Tpf2mpHost* host, int gog, int enabled) {
    const auto oldHost = H; const bool oldGog = g_gog, oldEnabled = g_terrainAlignFast;
    const auto oldOriginal = g_originalCalculateHeightMod;
    const uintptr_t oldBase = g_terrainAlignBase;
    H = host; g_gog = gog != 0; g_terrainAlignFast = enabled != 0;
    const bool result = InstallTerrainAlignFast();
    H = oldHost; g_gog = oldGog; g_terrainAlignFast = oldEnabled;
    g_originalCalculateHeightMod = oldOriginal; g_terrainAlignBase = oldBase;
    return result;
}
