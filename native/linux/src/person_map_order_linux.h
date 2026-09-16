#pragma once
#include <cstddef>
#include <cstdint>
// Original Windows build-35924 unordered-map iteration, including rehashes.
// Duplicate IDs do not create duplicate outputs. No game data is accessed.
bool Tpf2mpWindowsPersonMapOrder(const uint32_t* ids, size_t count,
                                uint32_t* output, size_t* outputCount);
bool Tpf2mpInstallPersonMapOrder(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpPersonMapOrderStatus();
using Tpf2mpPersonMapLog = void (*)(const char* format, ...);
// Set before game entry; each distinct runtime capture failure is logged once.
void Tpf2mpPersonMapOrderSetLog(Tpf2mpPersonMapLog log);
