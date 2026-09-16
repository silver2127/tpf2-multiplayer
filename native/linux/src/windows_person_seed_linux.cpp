#include "windows_person_seed_linux.h"

namespace {
constexpr uint64_t FnvInteger(uint32_t value)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    // MSVC hashes the four little-endian representation bytes, including for
    // negative int values. Work entirely with unsigned bits and defined wrap.
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (value >> shift) & 0xffu;
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}
constexpr uint64_t kTagHash = FnvInteger(4) + UINT64_C(0x9e3779b9);
constexpr uint64_t kTimeOffset = UINT64_C(0x9e3779b9) + (kTagHash << 6) + (kTagHash >> 2);
static_assert(kTagHash == UINT64_C(0xcd3ac65ee32e9b6a));
static_assert(kTimeOffset == UINT64_C(0x8200495122a9fb13));
constexpr uint64_t Combine(uint64_t seed, uint32_t value)
{
    return seed ^ (FnvInteger(value) + UINT64_C(0x9e3779b9) + (seed << 6) + (seed >> 2));
}
}

uint32_t Tpf2mpWindowsDepartureSeed(uint32_t nativeSeed)
{
    // Native: low32((sign-extended time + 0x2853a3c768) ^ 0x9e3779bd).
    // Both operations are invertible in uint32 arithmetic, even at the signed
    // boundaries; no time information is lost by the original truncation.
    const uint32_t timeBits = (nativeSeed ^ UINT32_C(0x9e3779bd)) - UINT32_C(0x53a3c768);
    return uint32_t((FnvInteger(timeBits) + kTimeOffset) ^ kTagHash);
}

uint32_t Tpf2mpWindowsArrivalSeed(uint32_t nativeSeed)
{
    const uint32_t timeBits = (nativeSeed ^ UINT32_C(0x9e3779bc)) - UINT32_C(0x53a3c728);
    return uint32_t(Combine(Combine(0, 3), timeBits));
}

uint32_t Tpf2mpWindowsTimeSeed(uint32_t tag, uint32_t timeBits)
{
    return uint32_t(Combine(Combine(0, tag), timeBits));
}

uint32_t Tpf2mpWindowsIdleSeed(uint32_t timeBits, uint32_t entityBits)
{
    return Tpf2mpWindowsEntitySeed(9, timeBits, entityBits);
}

uint32_t Tpf2mpWindowsEntitySeed(uint32_t tag, uint32_t timeBits, uint32_t entityBits)
{
    return uint32_t(Combine(Combine(Combine(0, tag), timeBits), entityBits));
}

uint32_t Tpf2mpWindowsEntitySeedFromNative(uint32_t tag, uint64_t intermediate,
                                        uint32_t entityBits, bool mixed)
{
    constexpr uint64_t k = UINT64_C(0x9e3779b9);
    if (mixed) {
        const int64_t entity = entityBits <= UINT32_C(0x7fffffff)
            ? int64_t(entityBits) : int64_t(entityBits) - (INT64_C(1) << 32);
        // S < 2^38 for every int32 time and verified tag, so S<<6 fits exactly.
        intermediate = (intermediate - uint64_t(entity) - k) >> 6;
    }
    const uint64_t first = k + tag;
    const uint64_t offset = k + (first << 6) + (first >> 2);
    const uint32_t timeBits = uint32_t((intermediate ^ first) - offset);
    return Tpf2mpWindowsEntitySeed(tag, timeBits, entityBits);
}
