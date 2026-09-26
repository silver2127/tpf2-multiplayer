// windows_ucrt_math_check.cpp -- WINDOWS-ONLY exhaustive check of
// src/windows_ucrt_math_linux.cpp against the real ucrtbase.dll, and the
// generator of tests/windows_ucrt_math_golden_linux.h for the Linux test.
//
//   cl /std:c++20 /O2 /EHsc /fp:precise tests\windows_ucrt_math_check.cpp
//      src\windows_ucrt_math_linux.cpp /Fe:ucrtcheck.exe
//   ucrtcheck.exe [golden-header-path]
//
// For each one-argument function every one of the 2^32 float inputs is
// compared bit for bit (NaN results compared by bits too). atan2f is checked
// on 2^31 random pairs plus edge grids. The golden header holds, per
// function, the FNV-1a-64 hash of ucrtbase's outputs over a fixed stride of
// inputs (atan2f: over 2^24 fixed pseudo-random pairs).
#include "../src/windows_ucrt_math_linux.h"
#include <windows.h>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

typedef float(__cdecl* F1)(float);
typedef float(__cdecl* F2)(float, float);

// std::bit_cast: MSVC 14.44 /O2 turns memcpy(float <- (uint32_t)u64 counter)
// into an int->float conversion.
static uint32_t B(float f) { return std::bit_cast<uint32_t>(f); }
static float F(uint32_t u) { return std::bit_cast<float>(u); }

struct Fn1 { const char* name; F1 ref; float (*mine)(float); };

static float MySin(float x) { return Tpf2mpUcrtSinf(x); }
static float MyCos(float x) { return Tpf2mpUcrtCosf(x); }
static float MyTan(float x) { return Tpf2mpUcrtTanf(x); }
static float MyAcos(float x) { return Tpf2mpUcrtAcosf(x); }

static uint64_t Fnv(uint64_t h, uint32_t v)
{
    for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 0x100000001b3ULL; }
    return h;
}
// Stride over all inputs used by the golden hash (prime, ~44M inputs).
static const uint32_t kStride = 97;

int main(int argc, char** argv)
{
    HMODULE u = LoadLibraryA("ucrtbase.dll");
    if (!u) { std::printf("no ucrtbase\n"); return 2; }
    Fn1 fns[] = {
        {"sinf", (F1)GetProcAddress(u, "sinf"), MySin},
        {"cosf", (F1)GetProcAddress(u, "cosf"), MyCos},
        {"tanf", (F1)GetProcAddress(u, "tanf"), MyTan},
        {"acosf", (F1)GetProcAddress(u, "acosf"), MyAcos},
    };
    F2 atan2ref = (F2)GetProcAddress(u, "atan2f");
    const unsigned threads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 8;
    int failures = 0;
    FILE* golden = argc > 1 ? std::fopen(argv[1], "wb") : nullptr;
    if (golden) {
        std::fprintf(golden, "// Generated on Windows by tests/windows_ucrt_math_check.cpp against\n"
                             "// ucrtbase.dll; do not edit. hash = FNV-1a-64 over the output bits of\n"
                             "// every input i*%u (i = 0 .. 2^32/%u), 4 bytes little endian each.\n#pragma once\n#include <cstdint>\n", kStride, kStride);
        std::fprintf(golden, "constexpr uint32_t kUcrtGoldenStride = %u;\n", kStride);
    }
    for (const Fn1& f : fns) {
        std::atomic<uint64_t> bad{0};
        std::atomic<uint32_t> firstBad{0xffffffffu};
        std::vector<std::thread> pool;
        const uint64_t total = 1ULL << 32;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&, t] {
                const uint64_t lo = total * t / threads, hi = total * (t + 1) / threads;
                uint64_t localBad = 0;
                for (uint64_t i = lo; i < hi; ++i) {
                    const float x = F((uint32_t)i);
                    const uint32_t a = B(f.ref(x)), b = B(f.mine(x));
                    if (a != b) {
                        if (!localBad) {
                            uint32_t cur = firstBad.load();
                            while ((uint32_t)i < cur && !firstBad.compare_exchange_weak(cur, (uint32_t)i)) {}
                        }
                        ++localBad;
                    }
                }
                bad += localBad;
            });
        }
        for (auto& th : pool) th.join();
        std::printf("%-6s all 2^32 inputs: %llu mismatches", f.name, (unsigned long long)bad.load());
        if (bad) {
            const uint32_t i = firstBad.load();
            std::printf("  first 0x%08x (%.9g): ucrt 0x%08x mine 0x%08x", i, F(i), B(f.ref(F(i))), B(f.mine(F(i))));
            ++failures;
        }
        std::printf("\n");
        if (golden) {
            uint64_t h = 0xcbf29ce484222325ULL;
            for (uint64_t i = 0; i < (1ULL << 32); i += kStride) h = Fnv(h, B(f.ref(F((uint32_t)i))));
            std::fprintf(golden, "constexpr uint64_t kUcrtGolden_%s = 0x%016llxULL;\n", f.name, (unsigned long long)h);
        }
    }
    // atan2f: random pairs over all bit patterns, plus a grid of edge values.
    {
        std::atomic<uint64_t> bad{0};
        std::atomic<uint64_t> firstPair{~0ULL};
        std::vector<std::thread> pool;
        const uint64_t perThread = (1ULL << 31) / threads;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&, t] {
                uint64_t s = 0x9e3779b97f4a7c15ULL * (t + 1);
                uint64_t localBad = 0;
                for (uint64_t i = 0; i < perThread; ++i) {
                    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                    uint32_t yb = (uint32_t)s, xb = (uint32_t)(s >> 32);
                    // Half of the pairs: game-like magnitudes (exponents 0x70..0x90).
                    if (i & 1) { yb = (yb & 0x807fffff) | ((0x70 + (yb >> 23) % 0x20) << 23);
                                 xb = (xb & 0x807fffff) | ((0x70 + (xb >> 23) % 0x20) << 23); }
                    const float y = F(yb), x = F(xb);
                    if (B(atan2ref(y, x)) != B(Tpf2mpUcrtAtan2f(y, x))) {
                        if (!localBad) firstPair = ((uint64_t)yb << 32) | xb;
                        ++localBad;
                    }
                }
                bad += localBad;
            });
        }
        for (auto& th : pool) th.join();
        const float edge[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-45f, -1e-45f, 1e-38f, 3e38f, -3e38f,
                              INFINITY, -INFINITY, NAN, 0.5f, 2.0f, 0.0625f, 1e-4f, 256.0f};
        uint64_t edgeBad = 0;
        for (float y : edge) for (float x : edge)
            if (B(atan2ref(y, x)) != B(Tpf2mpUcrtAtan2f(y, x))) {
                std::printf("  atan2f edge y=%g x=%g: ucrt 0x%08x mine 0x%08x\n", y, x,
                            B(atan2ref(y, x)), B(Tpf2mpUcrtAtan2f(y, x)));
                ++edgeBad;
            }
        std::printf("atan2f 2^31 random pairs: %llu mismatches, edge grid: %llu", (unsigned long long)bad.load(),
                    (unsigned long long)edgeBad);
        if (bad) {
            const uint64_t p = firstPair.load();
            const float y = F((uint32_t)(p >> 32)), x = F((uint32_t)p);
            std::printf("  e.g. y=0x%08x x=0x%08x ucrt 0x%08x mine 0x%08x", (uint32_t)(p >> 32), (uint32_t)p,
                        B(atan2ref(y, x)), B(Tpf2mpUcrtAtan2f(y, x)));
        }
        std::printf("\n");
        if (bad || edgeBad) ++failures;
        if (golden) {
            uint64_t h = 0xcbf29ce484222325ULL, s = 0x243f6a8885a308d3ULL;
            for (int i = 0; i < (1 << 24); ++i) {
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                h = Fnv(h, B(atan2ref(F((uint32_t)s), F((uint32_t)(s >> 32)))));
            }
            std::fprintf(golden, "// atan2f: 2^24 xorshift64 pairs from seed 0x243f6a8885a308d3 (y = low, x = high word).\n");
            std::fprintf(golden, "constexpr uint64_t kUcrtGolden_atan2f = 0x%016llxULL;\n", (unsigned long long)h);
        }
    }
    if (golden) std::fclose(golden);
    std::printf(failures ? "FAILED (%d functions)\n" : "all bit-identical\n", failures);
    return failures ? 1 : 0;
}
