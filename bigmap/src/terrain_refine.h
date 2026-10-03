// Steam 35924 sub_terrain_util::InternBicubicRefine at RVA 0x3ac6c0.
// Called once per BaseGetHeightmapRefined (0x3c4620) from TerrainAlignment
// ThreadPool workers; refines the 4 m base heightmap into the 1 m/2 m cache.
//
// Bit-identical replacement (docs/terrain-refine.md): every float value is
// produced by the same IEEE single-precision operation on the same operands
// in the same order as the stock code. What changed:
//  * i/k and j/k (and their squares/cubes) are computed once per call
//    instead of per cell (20 divisions per cell at k=4);
//  * the two CMat4f products (0x2fadc0) with the constant Hermite matrices are
//    inlined; only terms multiplied by exactly 0.0 or 1.0 are dropped. With
//    finite inputs such a drop can change nothing but the sign of a zero, and
//    no later +, -, * or the final truncation can observe that sign;
//  * scalar ops on independent values are packed 4-wide (mulps/addps are the
//    same per-lane operations as mulss/addss). No FMA, no reassociation.
// Cells are visited in stock order, each cell reads its samples (and the
// vector's data pointer) before writing its pixels, so even aliasing buffers
// behave like the original. Stock asserts and k > 64 go to the original.
#pragma once
#include <emmintrin.h>
#include <string.h>

struct BicubicRefineVector { const uint16_t* first; const uint16_t* last; const uint16_t* end; };
using BicubicRefineFn = void (__fastcall*)(int, const BicubicRefineVector*, int, int, int, int, int,
    const float*, uint16_t*, int, int, int);
static BicubicRefineFn g_originalBicubicRefine = nullptr;
static bool g_terrainRefineFast = false;
static const int kBicubicRefineMaxFactor = 64;

static void BicubicRefineImpl(BicubicRefineFn original, int k, const BicubicRefineVector* src,
    int srcDim, int x0, int y0, int x1, int y1, const float* scale, uint16_t* out,
    int stride, int dx, int dy)
{
    // Stock asserts: k > 1 && k % 2 == 0, x0 >= 0, y0 >= 0, x1 <= srcDim,
    // x1 >= x0, y1 >= y0. Those inputs keep the original's assert behaviour.
    if (k < 2 || (k & 1) || x0 < 0 || y0 < 0 || x1 > srcDim || x1 < x0 || y1 < y0 ||
        k > kBicubicRefineMaxFactor) {
        original(k, src, srcDim, x0, y0, x1, y1, scale, out, stride, dx, dy);
        return;
    }
    if (y0 >= y1 - 1 || x0 >= x1 - 1) return;   // stock loops run zero times

    // t = float(i)/float(k), t*t, (t*t)*t: the stock per-row and per-pixel values.
    alignas(16) float t1[kBicubicRefineMaxFactor + 4] = {};
    alignas(16) float t2[kBicubicRefineMaxFactor + 4] = {};
    alignas(16) float t3[kBicubicRefineMaxFactor + 4] = {};
    const __m128 kf = _mm_cvtsi32_ss(_mm_setzero_ps(), k);
    for (int i = 0; i < k; ++i) {
        const __m128 t = _mm_div_ss(_mm_cvtsi32_ss(_mm_setzero_ps(), i), kf);
        const __m128 tt = _mm_mul_ss(t, t);
        _mm_store_ss(t1 + i, t);
        _mm_store_ss(t2 + i, tt);
        _mm_store_ss(t3 + i, _mm_mul_ss(tt, t));
    }

    // Stock index arithmetic, including its 32-bit products and sign extension.
    const uint32_t uk = uint32_t(k), half = uint32_t(k >> 1);
    const uint32_t rowAdjust = uint32_t(dy) - uk * uint32_t(y0);
    const uint32_t colAdjust = uint32_t(dx) - uk * uint32_t(x0);
    const int64_t base0 = int32_t(uint32_t(srcDim) * uint32_t(y0) + uint32_t(x0));
    const int64_t off1 = int64_t(int32_t(uint32_t(y0 + 1) * uint32_t(srcDim) + uint32_t(x0))) - base0;
    const int64_t off2 = int64_t(int32_t(uint32_t(y0 + 2) * uint32_t(srcDim) + uint32_t(x0))) - base0;
    const uintptr_t outStep = uintptr_t(2 * int64_t(stride));

    const __m128 quarter = _mm_set1_ps(0.25f), halfF = _mm_set1_ps(0.5f);
    const __m128 m3 = _mm_set1_ps(-3.0f), p3 = _mm_set1_ps(3.0f);
    const __m128 m2 = _mm_set1_ps(-2.0f), p2 = _mm_set1_ps(2.0f);

    for (int y = y0; y < y1 - 1; ++y) {
        const int64_t row = base0 + int64_t(y - y0) * int64_t(srcDim);
        const uint32_t rowTerm = uk * uint32_t(y) + half + rowAdjust;
        for (int x = x0; x < x1 - 1; ++x) {
            const uintptr_t at = uintptr_t(src->first) + 2 * uintptr_t(row + (x - x0));
            const uint16_t* r0 = reinterpret_cast<const uint16_t*>(at);
            const uint16_t* r1 = reinterpret_cast<const uint16_t*>(at + 2 * uintptr_t(off1));
            const uint16_t* r2 = reinterpret_cast<const uint16_t*>(at + 2 * uintptr_t(off2));
            const float a00 = float(int(r0[0])), a01 = float(int(r0[1])), a02 = float(int(r0[2]));
            const float a10 = float(int(r1[0])), a11 = float(int(r1[1])), a12 = float(int(r1[2]));
            const float a20 = float(int(r2[0])), a21 = float(int(r2[1])), a22 = float(int(r2[2]));

            // G (stock 0x3ac920..0x3acb98), four lanes per operation:
            // S = G0,G1,G4,G5  H = G2,G3,G6,G7  V = G8,G9,G12,G13  X = G10,G11,G14,G15
            const __m128 tl = _mm_setr_ps(a00, a01, a10, a11);
            const __m128 tr = _mm_setr_ps(a01, a02, a11, a12);
            const __m128 bl = _mm_setr_ps(a10, a11, a20, a21);
            const __m128 br = _mm_setr_ps(a11, a12, a21, a22);
            const __m128 s = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_add_ps(tr, tl), bl), br), quarter);
            const __m128 hTop = _mm_sub_ps(tr, tl), hBottom = _mm_sub_ps(br, bl);
            const __m128 h = _mm_mul_ps(_mm_add_ps(hTop, hBottom), halfF);
            const __m128 xx = _mm_sub_ps(hBottom, hTop);
            const __m128 v = _mm_mul_ps(_mm_add_ps(_mm_sub_ps(bl, tl), _mm_sub_ps(br, tr)), halfF);
            const __m128 g0 = _mm_shuffle_ps(s, v, _MM_SHUFFLE(2, 0, 2, 0));   // G[r][0]
            const __m128 g1 = _mm_shuffle_ps(s, v, _MM_SHUFFLE(3, 1, 3, 1));   // G[r][1]
            const __m128 g2 = _mm_shuffle_ps(h, xx, _MM_SHUFFLE(2, 0, 2, 0));  // G[r][2]
            const __m128 g3 = _mm_shuffle_ps(h, xx, _MM_SHUFFLE(3, 1, 3, 1));  // G[r][3]

            // T = G*M1: T[r][c] = ((G[r][0]M1[0][c] + G[r][1]M1[1][c]) + G[r][2]M1[2][c]) + G[r][3]M1[3][c]
            // M1 columns: c0 (1,0,0,0) c1 (0,0,1,0) c2 (-3,3,-2,-1) c3 (2,-2,1,1)
            __m128 tc0 = g0, tc1 = g2;
            __m128 tc2 = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(g0, m3), _mm_mul_ps(g1, p3)),
                                               _mm_mul_ps(g2, m2)), g3);
            __m128 tc3 = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(g0, p2), _mm_mul_ps(g1, m2)), g2), g3);
            _MM_TRANSPOSE4_PS(tc0, tc1, tc2, tc3);   // now rows T[0..3][*]
            // C = M2*T: C[r][c] = ((M2[r][0]T[0][c] + M2[r][1]T[1][c]) + M2[r][2]T[2][c]) + M2[r][3]T[3][c]
            // M2 rows: (1,0,0,0) (0,0,1,0) (-3,3,-2,-1) (2,-2,1,1)
            const __m128 c0 = tc0, c1 = tc2;
            const __m128 c2 = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(tc0, m3), _mm_mul_ps(tc1, p3)),
                                                    _mm_mul_ps(tc2, m2)), tc3);
            const __m128 c3 = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(tc0, p2), _mm_mul_ps(tc1, m2)), tc2), tc3);

            const uint32_t pixel = rowTerm * uint32_t(stride) + uk * uint32_t(x) + half + colAdjust;
            uintptr_t dst = uintptr_t(out) + 2 * uintptr_t(int64_t(int32_t(pixel)));
            for (int i = 0; i < k; ++i, dst += outStep) {
                // P[c] = ((C[1][c]t + C[0][c]) + t2 C[2][c]) + C[3][c]t3
                const __m128 ti = _mm_set1_ps(t1[i]), ti2 = _mm_set1_ps(t2[i]), ti3 = _mm_set1_ps(t3[i]);
                const __m128 p = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(c1, ti), c0),
                                                       _mm_mul_ps(ti2, c2)), _mm_mul_ps(c3, ti3));
                const __m128 pp0 = _mm_shuffle_ps(p, p, _MM_SHUFFLE(0, 0, 0, 0));
                const __m128 pp1 = _mm_shuffle_ps(p, p, _MM_SHUFFLE(1, 1, 1, 1));
                const __m128 pp2 = _mm_shuffle_ps(p, p, _MM_SHUFFLE(2, 2, 2, 2));
                const __m128 pp3 = _mm_shuffle_ps(p, p, _MM_SHUFFLE(3, 3, 3, 3));
                const auto row16 = reinterpret_cast<uint16_t*>(dst);
                for (int j = 0; j < k; j += 4) {
                    // value = ((P1 s + P0) + s2 P2) + P3 s3, stored as the low
                    // 16 bits of cvttss2si (no saturation; 0x80000000 -> 0).
                    const __m128 sj = _mm_load_ps(t1 + j), sj2 = _mm_load_ps(t2 + j), sj3 = _mm_load_ps(t3 + j);
                    const __m128 value = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(pp1, sj), pp0),
                                                               _mm_mul_ps(sj2, pp2)), _mm_mul_ps(pp3, sj3));
                    const __m128i n = _mm_cvttps_epi32(value);
                    const __m128i wide = _mm_srai_epi32(_mm_slli_epi32(n, 16), 16);   // in int16 range
                    const __m128i low = _mm_packs_epi32(wide, wide);
                    if (k - j >= 4) {
                        _mm_storel_epi64(reinterpret_cast<__m128i*>(row16 + j), low);
                    } else {
                        const int32_t two = _mm_cvtsi128_si32(low);   // k is even: tail is 2
                        memcpy(row16 + j, &two, 4);
                    }
                }
            }
        }
    }
}

static void __fastcall BicubicRefineDetour(int k, const BicubicRefineVector* src, int srcDim,
    int x0, int y0, int x1, int y1, const float* scale, uint16_t* out, int stride, int dx, int dy)
{
    BicubicRefineImpl(g_originalBicubicRefine, k, src, srcDim, x0, y0, x1, y1, scale, out, stride, dx, dy);
}

static const uint8_t kBicubicRefinePrologue[20] = {
    0x40,0x55,0x53,0x41,0x57,                   // push rbp; push rbx; push r15
    0x48,0x8d,0xac,0x24,0xc0,0xfd,0xff,0xff,    // lea rbp,[rsp-0x240]
    0x48,0x81,0xec,0x40,0x03,0x00,0x00          // sub rsp,0x340
};
// Everything the replacement reproduces: the function body, its CMat4f
// product and the RIP-relative constants it loads. Generated from the Steam
// 35924 executable; tools/test_terrain_refine.py checks every byte.
struct BicubicRefineRegion { uintptr_t rva; uint32_t size; const char* hex; };
static const BicubicRefineRegion kBicubicRefineRegions[] = {
    {0x3ac6d4,2172, // InternBicubicRefine body after the hook prologue
        "488b05ddfae1034833c448898560010000488b85a0020000458bd94c63bda80200008bd9448b95b802000044894c2438448b8db0020000488945b8488955b083f9010f8e8e07"
        "0000f6c3010f85850700004585db0f885b0700008b8d8002000085c90f88f20700008b8588020000413bc00f8fc2070000413bc30f8c980700008b95900200003bd10f8c6907"
        "00000f2805c7b6bf020f280de0b6bf024889bc2430030000418bfb0faffb0f2985a00000000f280584b6bf020f298db00000000f280d66b6bf02442bcf4c89a424280300000f"
        "2985c0000000448be30f2805ea43b702440fafe10f298dd00000000f280da842b7020f2985e00000000f28053a7fc002452bd40f298df00000000f280d197fc002ffca897c24"
        "4044894c243444895424300f2985000100000f298d100100003bca0f8d51060000448d50ff4889b424380300004c89ac2420030000418bc00fafc14c89b424180300000f29b4"
        "24000300000f29bc24f0020000440f298424e00200004103c3440f298c24d00200004c63e8440f299424c00200004963c048894424484803c048894424504e8d0c6d04000000"
        "440f299c24b00200008d4102410fafc0440f29a424a0020000440f29ac2490020000440f29b42480020000440f29bc24700200004103c3448954243c4c63f08d4101410fafc0"
        "4c896c24584c894c24604103c34c63c04d2bc64d2bf52bd14c8945d0488b4c24488bc2488b54245048894424684c8975c890453bda0f8dd30400004b8d0c2e498bf1488bc145"
        "8bf2492bc5492bcd4c03c04c8be94c8945c0452bf30f1f8000000000488b45b0488b100fb74416fc66440f6ec80fb74416fe450f5bc9660f6ed00fb704160f5bd266440f6ee0"
        "488d0416420fb74c40fc440f28c2488d0416450f5be4660f6ec9f3450f5cc1420fb74c40fe410f28f40f5bc9488d0416660f6ee9f30f5cf2420fb70c40488d0416f30f114c24"
        "200f5bed660f6ec1420fb74c68fc440f28f50f5bc0488d0416f3440f5cf166440f6ef9420fb74c68fe0f28e5f30f11442424f30f5ce2440f28d8488d04160f28c2f3440f5cdd"
        "f3410f58c1660f6ed9420fb70c680f5bdbf30f58c166440f6ed1410f28cc440f28ebf30f58ca0f28fbf30f10542424f30f5cfd450f5bfff30f58c5f30f58cd450f5bd2f30f59"
        "05f33fb702f3450f5ceff30f58caf3440f11542428f3440f5cd3f30f114560410f28c0f3410f58c6f30f590dc83fb702f30f5905341fb702f30f114d640f28cef3410f58cbf3"
        "0f1145680f28c5f30f58442420f30f590d111fb702f3410f58c7f30f114d6c0f28caf30f58c3f30f5905843fb702f30f114570f30f10542428f30f58cdf30f106c24244c8d45"
        "60410f28c6488d95a0000000f3410f58c5f3450f5ceef30f58cbf30f105c2420f3440f5cfb488d4dd8f30f5905af1eb702f30f58caf3440f11ad98000000f30f5cd5f30f1145"
        "78f3440f58ff0f28c3f3410f5cc1f3440f100d831eb702f30f590d073fb702f30f58d7f3450f59f9f30f114d74f30f58c4410f28cbf3410f58caf3440f11bd90000000f3410f"
        "59d1f3450f5cd3f3410f59c1f30f590d401eb702f30f118580000000410f28c6f3410f5cc0f30f119594000000f30f114d7c0f28cdf3410f5cccf3440f11959c000000f30f11"
        "8588000000f30f58ccf3410f59c9f30f118d84000000410f28cbf30f5ccef30f118d8c000000e81be2f4ff4c8d85e0000000488d9520010000488d4d180f10000f1048100f29"
        "85200100000f1040200f298d300100000f1048300f2985400100000f298d50010000e8d9e1f4ff4533c00f10000f1048100f294424700f294d800f1040200f1048300f294590"
        "0f294da085db0f8e770100004d8bd766440f6ecb4d03d28bcb450f5bc985db79038d4b01f3440f1055acf3440f105da8f3440f1065a4f3440f106da0f3440f10759cf3440f10"
        "7d98d1f9418d040c03442430410fafc703c703c1488b4db80344243448984c8d0c416690f30f106d800f57d2f30f107584410f28cdf30f107d8833c9f3440f10458c498bd1f3"
        "410f2ad0f3410f5ed10f28e2f30f59eaf30f59e2f30f586c2470f30f59f20f28c4f30f59faf30f5945900f28dcf30f58742474f30f587c2478f30f58e8f30f59da0f28c4f344"
        "0f59c2f30f594594f3440f5844247cf30f59cbf30f58f00f28c4f3410f59c7f30f58e9f3410f59e6f30f58f8410f28ccf30f59cb410f28c2f3440f58c4f30f59c3f30f58f141"
        "0f28cbf30f59cbf3440f58c0f30f58f96690660f6ec10f28d60f5bc0ffc1f3410f5ec1f30f59d00f28d8f30f59d8f30f58d50f28cbf30f59dff30f59c8410f28c0f30f58d3f3"
        "0f59c1f30f58d0f30f2cc26689024883c2023bcb7cb641ffc04d03ca443bc30f8ce7feffff4c8b45c04883c60203fb4983ee010f8583fbffff4c8b6c2458448b5c24384c8b4c"
        "2460488b442468448b54243c8b7c24404c8b75c84c8b45d0488b4c2448488b5424504c03e94c03ca4403e34c896c24584883e8014c894c246048894424680f8502fbffff440f"
        "28bc2470020000440f28b42480020000440f28ac2490020000440f28a424a0020000440f289c24b0020000440f289424c0020000440f288c24d0020000440f288424e0020000"
        "0f28bc24f00200000f28b424000300004c8bb424180300004c8bac2420030000488bb42438030000488bbc24300300004c8ba42428030000488b8d600100004833cce8b36b84"
        "024881c440030000415f5b5dc34c8d0db075c00241b858000000488d15d374c002488d0d4c75c002e847dfe601cc4c8d0d8f75c00241b856000000488d15b274c002488d0d43"
        "76c002e826dfe601cc4c8d0d6e75c00241b85d000000488d159174c002488d0d4a75c002e805dfe601cc4c8d0d4d75c00241b85c000000488d157074c002488d0d1975c002e8"
        "e4dee601cc4c8d0d2c75c00241b85a000000488d154f74c002488d0df875c002e8c3dee601cc4c8d0d0b75c00241b859000000488d152e74c002488d0daf74c002e8a2dee601"
        "cccc"
    },
    {0x2fadc0,507, // CMat4f product called twice per cell
        "4883ec28f3410f104834f3410f104024f3410f1050140f14d1f3410f104830f3410f1020f3410f10680cf30f105a080f29742410f3410f1070080f293c24f3410f1078040f14"
        "f8f3410f1040200f14faf3410f1050100f14d1f3410f1048380f14e0f3410f1040280f14e2f3410f1050180f14d1f3410f10483c0f14f0f3410f10402c0f14f2f3410f10501c"
        "0f14d1f30f104a200f14e8f30f10020f14eaf30f1052100fc6d2000f59d70fc6c0000f59c40fc6c9000f59ce0f58d00fc6db00f30f1042300fc6c0000f59c50f58d10f59dcf3"
        "0f104a240fc6c9000f59ce0f58d0f30f1042040fc6c0000f59c4f30f11110fc6d2e5f30f1151100f15d2f30f1151200f15d2f30f115130f30f1052140fc6d2000f59d70f58d0"
        "f30f1042340fc6c0000f59c50f58d10f58d0f30f1042180fc6c0000f59c7f30f1151040fc6d2e5f30f1151140f58d80f15d2f30f1151240f15d2f30f115134f30f104a28488b"
        "c1f30f104238f30f10520c0fc6c0000f59c50fc6d2000f59d40fc6c9000f59ce0f58d9f30f104a2c0fc6c9000f59ce0f287424100f58d8f30f10421c0fc6c0000f59c70f283c"
        "24f30f1159080fc6dbe50f58d0f30f115918f30f10423c0f15db0fc6c0000f59c50f58d1f30f1159280f15dbf30f1159380f58d0f30f11510c0fc6d2e5f30f11511c0f15d2f3"
        "0f11512c0f15d2f30f11513c4883c428c3"
    },
    {0x2f1e988,4, // 0.5f
        "0000003f"
    },
    {0x2f20a14,4, // 0.25f
        "0000803e"
    },
    {0x2f20a70,16, // M2 row 1
        "00000000000000000000803f00000000"
    },
    {0x2f20ba0,16, // M2 row 0
        "0000803f000000000000000000000000"
    },
    {0x2fa7e00,32, // M1 rows 3 and 2
        "0000000000000000000080bf0000803f000000000000803f000000c00000803f"
    },
    {0x2fa7e30,16, // M1 row 0
        "0000803f00000000000040c000000040"
    },
    {0x2fa7e50,16, // M1 row 1
        "000000000000000000004040000000c0"
    },
    {0x2fb4700,32, // M2 rows 3 and 2
        "00000040000000c00000803f0000803f000040c000004040000000c0000080bf"
    },
};

static bool VerifyBicubicRefineRegion(const BicubicRefineRegion& r) {
    uint8_t bytes[2200];
    if (r.size > sizeof bytes || strlen(r.hex) != size_t(r.size) * 2) return false;
    for (uint32_t i = 0; i < r.size; ++i) {
        int v = 0;
        for (int n = 0; n < 2; ++n) {
            const char c = r.hex[2 * i + n];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -256);
        }
        if (v < 0) return false;
        bytes[i] = uint8_t(v);
    }
    return H->verifyBytes(r.rva, bytes, r.size) != 0;
}

static bool InstallTerrainRefineFast() {
    if (!g_terrainRefineFast || g_gog) return false;
    if (!H->verifyBytes(0x3ac6c0, kBicubicRefinePrologue, sizeof kBicubicRefinePrologue)) {
        H->log("terrain refine fast: prologue mismatch; OFF"); return false;
    }
    for (const auto& region : kBicubicRefineRegions) {
        if (!VerifyBicubicRefineRegion(region)) {
            H->log("terrain refine fast: byte mismatch at RVA 0x%llx; OFF", (unsigned long long)region.rva);
            return false;
        }
    }
    void* original = nullptr;
    if (!H->installHook(H->moduleBase() + 0x3ac6c0, (void*)&BicubicRefineDetour,
                        sizeof kBicubicRefinePrologue, &original)) {
        H->log("terrain refine fast: hook failed; OFF"); return false;
    }
    g_originalBicubicRefine = reinterpret_cast<BicubicRefineFn>(original);
    H->log("terrain refine fast: bit-identical SSE2 height refinement enabled (stock asserts and k > 64 use the original)");
    return true;
}

extern "C" __declspec(dllexport)
void BigmapTestBicubicRefine(BicubicRefineFn original, int k, const BicubicRefineVector* src, int srcDim,
    int x0, int y0, int x1, int y1, const float* scale, uint16_t* out, int stride, int dx, int dy) {
    BicubicRefineImpl(original, k, src, srcDim, x0, y0, x1, y1, scale, out, stride, dx, dy);
}
extern "C" __declspec(dllexport)
int BigmapTestInstallTerrainRefine(const Tpf2mpHost* host, int gog, int enabled) {
    const auto oldHost = H; const bool oldGog = g_gog, oldEnabled = g_terrainRefineFast;
    const auto oldOriginal = g_originalBicubicRefine;
    H = host; g_gog = gog != 0; g_terrainRefineFast = enabled != 0;
    const bool result = InstallTerrainRefineFast();
    H = oldHost; g_gog = oldGog; g_terrainRefineFast = oldEnabled; g_originalBicubicRefine = oldOriginal;
    return result;
}
