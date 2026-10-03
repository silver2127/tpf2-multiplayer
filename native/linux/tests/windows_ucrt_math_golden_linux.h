// Generated on Windows by tests/windows_ucrt_math_check.cpp against
// ucrtbase.dll; do not edit. hash = FNV-1a-64 over the output bits of
// every input i*97 (i = 0 .. 2^32/97), 4 bytes little endian each.
#pragma once
#include <cstdint>
constexpr uint32_t kUcrtGoldenStride = 97;
constexpr uint64_t kUcrtGolden_sinf = 0xd8a999a22a114357ULL;
constexpr uint64_t kUcrtGolden_cosf = 0x429f4cb954f63d0eULL;
constexpr uint64_t kUcrtGolden_tanf = 0xc9febfd84b0d75a6ULL;
constexpr uint64_t kUcrtGolden_acosf = 0xa5bdb4f8091c8b4fULL;
// atan2f: 2^24 xorshift64 pairs from seed 0x243f6a8885a308d3 (y = low, x = high word).
constexpr uint64_t kUcrtGolden_atan2f = 0x1f03a9f1fccae2a3ULL;
