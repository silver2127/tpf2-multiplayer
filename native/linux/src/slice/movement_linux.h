#pragma once
#include <cstdint>
// Called only after the slice's build, read and no-patches gates.
bool SliceInstallMovement(uintptr_t base, const char* root, const char* data);
