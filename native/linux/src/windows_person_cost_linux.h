#pragma once
#include <cstdint>

// Windows build35924's deterministic walking/driving cost multipliers. The
// input is the same person identifier passed to the original native helper.
// Hash entry points let inline adapters retain the game's modulo and SSE code.
uint64_t Tpf2mpWindowsWalkCostHash(uint32_t person);
uint64_t Tpf2mpWindowsDriveCostHash(uint32_t person);
float Tpf2mpWindowsWalkCost(uint32_t input);
float Tpf2mpWindowsDriveCost(uint32_t input);

// Windows hashes the person/line as LE32 and the stop index as LE16.
uint64_t Tpf2mpWindowsLineCostHash(uint32_t person, uint32_t line, uint16_t stop);

// Inputs and combined hash for PathFactory's (time, batch) and
// (person, entity revision) pairs; exposed for instruction-level tests.
uint64_t Tpf2mpWindowsPathHashInput(uint32_t value);
uint64_t Tpf2mpWindowsPathHash(uint32_t first, uint32_t second);
