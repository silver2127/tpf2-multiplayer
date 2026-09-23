#include "windows_person_cost_linux.h"

namespace {
uint64_t HashInteger(uint32_t input)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (input >> shift) & 0xffu;
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

uint64_t CostHash(uint32_t input, uint64_t salt)
{
    const uint64_t hash = HashInteger(input);
    const uint64_t seed = hash + UINT64_C(0x9e3779b9);
    return seed ^ ((hash << 6) + (seed >> 2) + salt);
}

float Cost(uint64_t mixed)
{
    // Windows executes separate CVTSI2SS, MULSS and ADDSS instructions. Force
    // the intermediate float rounding even if a future build enables FMA.
    volatile float scaled = float(mixed % 10000u) * 0.0001f;
    return scaled + 0.5f;
}
}

uint64_t Tpf2mpWindowsWalkCostHash(uint32_t person)
{
    return CostHash(person, UINT64_C(0x3bfa9643a97b9c08));
}

uint64_t Tpf2mpWindowsDriveCostHash(uint32_t person)
{
    return CostHash(person, UINT64_C(0xceb8a94acd0807a8));
}

float Tpf2mpWindowsWalkCost(uint32_t input)
{
    return Cost(Tpf2mpWindowsWalkCostHash(input));
}

float Tpf2mpWindowsDriveCost(uint32_t input)
{
    return Cost(Tpf2mpWindowsDriveCostHash(input));
}

uint64_t Tpf2mpWindowsLineCostHash(uint32_t person, uint32_t line, uint16_t stop)
{
    uint64_t seed = HashInteger(person) + UINT64_C(0x9e3779b9);
    seed ^= HashInteger(line) + UINT64_C(0x9e3779b9) + (seed << 6) + (seed >> 2);
    uint64_t stopHash = UINT64_C(0xcbf29ce484222325);
    for (unsigned shift = 0; shift < 16; shift += 8) {
        stopHash ^= (stop >> shift) & 0xffu;
        stopHash *= UINT64_C(0x100000001b3);
    }
    return seed ^ (stopHash + UINT64_C(0x9e3779b9) + (seed << 6) + (seed >> 2));
}

uint64_t Tpf2mpWindowsPathHashInput(uint32_t value)
{
    return HashInteger(value) + UINT64_C(0x9e3779b9);
}

uint64_t Tpf2mpWindowsPathHash(uint32_t first, uint32_t second)
{
    const uint64_t seed = Tpf2mpWindowsPathHashInput(first);
    return seed ^ (Tpf2mpWindowsPathHashInput(second) + (seed << 6) + (seed >> 2));
}
