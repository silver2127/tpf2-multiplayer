#pragma once
#include <cstdint>

uint64_t Tpf2mpWindowsResidentHash(uint32_t id) noexcept;
bool Tpf2mpInstallResidentHash(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpResidentHashStatus();
