#pragma once
#include <cstdint>

// Windows build 35924's uniform-int distribution, inclusive at both ends.
// Preconditions: minimum <= maximum and next != nullptr. The callback returns
// one unmodified MT19937 uint32_t draw and advances its original engine.
// A singleton range consumes no draws. No engine layout is assumed here.
int32_t Tpf2mpWindowsUniformIntInclusive(int32_t minimum, int32_t maximum,
                                       void* engine, uint32_t (*next)(void*));
