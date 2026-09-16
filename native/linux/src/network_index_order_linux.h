#pragma once
#include <cstdint>
bool Tpf2mpInstallNetworkIndexOrder(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpNetworkIndexOrderStatus();
using Tpf2mpNetworkIndexOrderLog = void (*)(const char* format, ...);
void Tpf2mpNetworkIndexOrderSetLog(Tpf2mpNetworkIndexOrderLog log);
