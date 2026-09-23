#pragma once
#include <cstdint>

// Only verified animal spawn and movement engines receive Windows std-MT
// streams. Other native LCG engines retain their original behavior.
bool Tpf2mpInstallAnimalRng(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpAnimalRngStatus();
