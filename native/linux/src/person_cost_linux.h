#pragma once
#include <cstdint>

// Constructor-stage only: replace the eight verified travel-cost/seed
// integer hashes, retaining the game's original modulo and floating arithmetic.
bool Tpf2mpInstallPersonCosts(uintptr_t imageBase, const char* buildId);

// Includes any partial installation/rollback failure; do not report merely
// "off" when a constructor-stage write could not be restored.
const char* Tpf2mpPersonCostStatus();
