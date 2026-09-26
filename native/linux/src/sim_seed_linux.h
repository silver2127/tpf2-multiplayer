#pragma once
#include <cstdint>

// Windows seed hashing for eight simulation MT seeds that the person, town,
// path-cost, animal and tree adapters do not cover (build 35924). Windows
// seeds these engines with hash_combine over MSVC std::hash (FNV-1a); the
// native build hashes int with the identity. Each verified site is rewritten
// only at the instruction that stores or finishes the seed; the clock, the MT
// initializer and every draw stay native. TPF2MP_SIM_SEED=0 leaves them off.
bool Tpf2mpInstallSimSeeds(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpSimSeedStatus();

// Pure pieces, exposed for tests.
enum class Tpf2mpSimSeedKind : uint8_t {
    FnvRax,       // seed = low32(FNV-1a(time)) (std::hash<int>), time in eax
    TimeRdx,      // seed = hash(tag, time); edx holds the native identity seed
    MixedRaxRdx,  // seed = hash(tag, time, entity); rax = e + k + (S << 6)
    S1RcxRax,     // seed = hash(tag, time, entity); rcx = S, rax ^= rcx follows
};
struct Tpf2mpSimSeedRegs { uint64_t rax, rcx, rdx; };
// Rewrite the registers exactly as the dispatch does for one verified site.
void Tpf2mpSimSeedTransform(Tpf2mpSimSeedKind kind, uint32_t tag, uint32_t entity,
                            Tpf2mpSimSeedRegs* regs);
