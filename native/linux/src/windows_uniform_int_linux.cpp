#include "windows_uniform_int_linux.h"
#include <cassert>

int32_t Tpf2mpWindowsUniformIntInclusive(int32_t minimum, int32_t maximum,
                                       void* engine, uint32_t (*next)(void*))
{
    assert(minimum <= maximum && next);
    // Compute the inclusive width in 64 bits: the full signed range has
    // 2^32 outcomes, and subtracting its bounds as signed int would overflow.
    const uint64_t width = uint64_t(int64_t(maximum) - int64_t(minimum)) + 1;
    if (width == 1) return minimum;
    const uint64_t limit = ((uint64_t{1} << 32) / width) * width;
    uint32_t raw;
    do raw = next(engine); while (uint64_t(raw) >= limit);
    return int32_t(int64_t(minimum) + int64_t(uint64_t(raw) % width));
}
