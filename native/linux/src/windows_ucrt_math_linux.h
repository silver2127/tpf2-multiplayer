#pragma once
#include <cstdint>

// Bit-exact models of the Windows UCRT (ucrtbase.dll) float math that the
// Windows TransportFever2.exe imports (api-ms-win-crt-math-l1-1-0), so the
// native Linux game can compute exactly what a Windows peer computes.
//
// Why: glibc's sinf/cosf/acosf/tanf are more accurate than the UCRT ones and
// round differently in about 1-2% of calls (measured on game-like inputs);
// the town street developer and TownDeveloper feed these results into
// integer truncations and distance/angle thresholds, so one ulp is enough to
// build a street on one platform and not on the other.
//
// Source of truth: ucrtbase.dll 10.0.26100 (the algorithms descend from AMD's
// open libm; the tables are mathematical constants):
//   sinf  0xac360 -> 0xac36d (AVX2+FMA)    Taylor sin/cos on |r| <= pi/4,
//   cosf  0xa7f70 -> 0xa7f7d (AVX2+FMA)    Cody-Waite pi/2 reduction, and a
//                                          Payne-Hanek reduction for huge |x|
//   tanf  0xad740 -> 0xad420 (one path)    rational approximation
//   acosf 0x73890 -> 0x738ac (AVX2+FMA)    single-precision FMA evaluation
//   atan2f 0x51610 -> 0x519c0 (SSE2)       atan(k/256) table + 3rd-order term
// ucrtbase picks a path per CPU. Its AVX2+FMA and SSE2 paths DIFFER for sinf
// (44,232 of 2^32 inputs, mostly |x| >= 2^23), cosf (18,018, incl. tiny |x|)
// and acosf (about 0.2% of inputs); tanf has one path and atan2f's two agree
// on 2^31 pairs. The models follow the AVX2+FMA paths, which every Windows
// CPU with AVX2 and FMA3 takes (a Windows peer without them already differs
// from other Windows peers).
//
// Every function here is checked bit for bit against ucrtbase.dll over all
// 2^32 inputs (atan2f: 2^31 random pairs plus edge grids) by
// tests/windows_ucrt_math_check.cpp, which builds on Windows; the Linux test
// replays golden vectors produced there.
//
// Compile with -ffp-contract=off (no FMA contraction): every double/float
// operation must be the single IEEE operation the UCRT executes.
float Tpf2mpUcrtSinf(float x);
float Tpf2mpUcrtCosf(float x);
void Tpf2mpUcrtSincosf(float x, float* s, float* c);
float Tpf2mpUcrtTanf(float x);
float Tpf2mpUcrtAcosf(float x);
float Tpf2mpUcrtAtan2f(float y, float x);
