#pragma once
#include <cstdint>
bool Tpf2mpInstallTargetOrder(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpTargetOrderStatus();
using Tpf2mpTargetOrderLog = void (*)(const char*, ...);
void Tpf2mpTargetOrderSetLog(Tpf2mpTargetOrderLog log);
