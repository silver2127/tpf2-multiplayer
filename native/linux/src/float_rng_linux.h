#pragma once
#include <cstdint>

// Match the verified Windows MT unit-float sampler's rounded upper endpoint.
bool Tpf2mpInstallFloatRng(uintptr_t imageBase, const char* buildId);
