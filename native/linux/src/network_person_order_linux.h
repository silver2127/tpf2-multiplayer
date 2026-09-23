#pragma once
#include <cstdint>

// Matches Windows iteration of the temporary affected-person and affected-network sets.
// Native set contents, node links and engine APIs remain unchanged.
bool Tpf2mpInstallNetworkPersonOrder(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpNetworkPersonOrderStatus();
using Tpf2mpNetworkPersonLog = void (*)(const char* format, ...);
void Tpf2mpNetworkPersonOrderSetLog(Tpf2mpNetworkPersonLog log);
