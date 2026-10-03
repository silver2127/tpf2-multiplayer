#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
// Call only after the build-id, slice read backend and no-patches gates pass.
bool SliceInstallTrainOrder(uintptr_t base, const char* rootDir, const char* dataDir);
void SliceTrainOrderLogAlive();
uintptr_t SliceNameComponent(uintptr_t world, int32_t id, int type);
// The names SliceNameComponent + SliceReadStdString would give each id one at a
// time (empty where that fails), read a level at a time for all of them.
void SliceEntityNames(uintptr_t world, int type, const int32_t* ids, size_t n, std::vector<std::string>* names);
