#pragma once
#include <cstdint>
// Call only after the build-id, slice read backend and no-patches gates pass.
bool SliceInstallTrainOrder(uintptr_t base, const char* rootDir, const char* dataDir);
void SliceTrainOrderLogAlive();
