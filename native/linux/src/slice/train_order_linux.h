#pragma once
#include <cstdint>
// Call only after the build-id, slice read backend and no-patches gates pass.
bool SliceInstallTrainOrder(uintptr_t base, const char* rootDir, const char* dataDir);
void SliceTrainOrderLogAlive();
uintptr_t SliceNameComponent(uintptr_t world, int32_t id, int type);
