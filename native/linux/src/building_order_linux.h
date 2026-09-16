#pragma once
#include <cstddef>
#include <cstdint>

struct Tpf2mpBuildingOrderKey { int32_t priority, year; };
using Tpf2mpBuildingOrderLookup = Tpf2mpBuildingOrderKey (*)(void*, int32_t);
// Reconstruct ConstructionRep's initial ascending-ID enumeration, then apply
// build-35924 Windows ordering, including its equal-key partition behavior.
void Tpf2mpWindowsBuildingOrder(int32_t* ids, size_t count, void* context,
                               Tpf2mpBuildingOrderLookup lookup);
bool Tpf2mpInstallBuildingOrder(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpBuildingOrderStatus();
