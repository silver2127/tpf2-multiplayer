#pragma once
#include <cstddef>
#include <cstdint>

// MSVC STL algorithms of Windows build 35924, over a 32-bit engine whose raw
// draws come from `next` (one unmodified uint32 per call, engine advanced).
// Verified against the Windows executable's own code under emulation
// (scratchpad/emu_engines.py, gen_engine_golden.py): uniform_int<size_t>
// 0x140b6cf80, std::shuffle 0x140914e80 / 0x1402ab740, Random<std::mt>
// 0x142374e10.
using Tpf2mpRawNext = uint32_t (*)(void*);

// _Rng_from_urng<uint64>::operator()(index): a value in [0, index), index >= 1.
uint64_t Tpf2mpMsvcRngFromUrng(uint64_t index, void* engine, Tpf2mpRawNext next);
// uniform_int_distribution<uint64/uint32>(minimum, maximum) with maximum >= minimum.
uint64_t Tpf2mpMsvcUniformU64(uint64_t minimum, uint64_t maximum, void* engine, Tpf2mpRawNext next);
uint32_t Tpf2mpMsvcUniformU32(uint32_t minimum, uint32_t maximum, void* engine, Tpf2mpRawNext next);
// std::shuffle(first, first + count) for trivially copyable elements of elementSize bytes.
void Tpf2mpMsvcShuffle(void* first, size_t count, size_t elementSize, void* engine, Tpf2mpRawNext next);
// generate_canonical<float, 24> of one 32-bit draw (no clamp: a rounded 1.0 stays 1.0).
float Tpf2mpMsvcUnitFloat(uint32_t raw);
// generate_canonical<float, 24> of one std::minstd_rand draw g in [1, 2^31-2].
float Tpf2mpMsvcMinstdUnitFloat(uint32_t g);

// Compact mt19937 used as a Windows std::mt19937 sidecar (raw outputs only).
struct Tpf2mpMt19937 {
    uint32_t state[624];
    uint32_t index;
};
void Tpf2mpMtSeed(Tpf2mpMt19937* mt, uint32_t seed);
uint32_t Tpf2mpMtNext(void* mt);
