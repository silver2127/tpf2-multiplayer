#include "windows_msvc_random_linux.h"
#include <cstring>

uint64_t Tpf2mpMsvcRngFromUrng(uint64_t index, void* engine, Tpf2mpRawNext next)
{
    // MSVC _Rng_from_urng: gather whole 32-bit draws until the mask covers
    // index-1, then accept unless the value falls into the partial last bucket.
    for (;;) {
        uint64_t value = 0, mask = 0;
        while (mask < index - 1) {
            value = (value << 32) | next(engine);
            mask = (mask << 32) | UINT64_C(0xffffffff);
        }
        if (value / index < mask / index || mask % index == index - 1) return value % index;
    }
}

uint64_t Tpf2mpMsvcUniformU64(uint64_t minimum, uint64_t maximum, void* engine, Tpf2mpRawNext next)
{
    const uint64_t span = maximum - minimum;
    if (span == UINT64_MAX) {
        // _Get_all_bits: two draws, first one in the high half.
        const uint64_t high = next(engine);
        return (high << 32) | next(engine);
    }
    return minimum + Tpf2mpMsvcRngFromUrng(span + 1, engine, next);
}

uint32_t Tpf2mpMsvcUniformU32(uint32_t minimum, uint32_t maximum, void* engine, Tpf2mpRawNext next)
{
    const uint32_t span = maximum - minimum;
    if (span == UINT32_MAX) return next(engine);
    return minimum + uint32_t(Tpf2mpMsvcRngFromUrng(uint64_t(span) + 1, engine, next));
}

void Tpf2mpMsvcShuffle(void* first, size_t count, size_t elementSize, void* engine, Tpf2mpRawNext next)
{
    // MSVC _Random_shuffle1: forward Fisher-Yates, element i swaps with a
    // draw in [0, i]; no swap when the draw is i itself.
    auto* base = static_cast<unsigned char*>(first);
    unsigned char tmp[64];
    if (elementSize > sizeof(tmp)) return;
    for (size_t i = 1; i < count; ++i) {
        const uint64_t j = Tpf2mpMsvcRngFromUrng(uint64_t(i) + 1, engine, next);
        if (j == i) continue;
        unsigned char* a = base + i * elementSize;
        unsigned char* b = base + j * elementSize;
        std::memcpy(tmp, a, elementSize);
        std::memcpy(a, b, elementSize);
        std::memcpy(b, tmp, elementSize);
    }
}

float Tpf2mpMsvcUnitFloat(uint32_t raw)
{
    // One draw (ceil(24 / log2(2^32)) == 1): (float(raw) - 0) * 1, then / 2^32.
    volatile float converted = float(uint64_t(raw));
    volatile float zero = 0.0f;
    volatile float sum = zero + (converted - zero);
    return sum / 0x1p32f;
}

float Tpf2mpMsvcMinstdUnitFloat(uint32_t g)
{
    // minstd_rand: min 1, range 2^31-2 (float 2^31). One draw:
    // (float(g) - 1.0f) * 1.0f accumulated from 0, divided by float(range).
    volatile float converted = float(uint64_t(g));
    volatile float one = 1.0f;
    volatile float sum = 0.0f + (converted - one) * one;
    return sum / 0x1p31f;
}

void Tpf2mpMtSeed(Tpf2mpMt19937* mt, uint32_t seed)
{
    mt->state[0] = seed;
    for (uint32_t i = 1; i < 624; ++i)
        mt->state[i] = 0x6c078965u * (mt->state[i - 1] ^ (mt->state[i - 1] >> 30)) + i;
    mt->index = 624;
}

uint32_t Tpf2mpMtNext(void* opaque)
{
    auto* mt = static_cast<Tpf2mpMt19937*>(opaque);
    if (mt->index >= 624) {
        for (uint32_t k = 0; k < 624; ++k) {
            const uint32_t y = (mt->state[k] & 0x80000000u) | (mt->state[(k + 1) % 624] & 0x7fffffffu);
            mt->state[k] = mt->state[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        }
        mt->index = 0;
    }
    uint32_t y = mt->state[mt->index++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}
