#pragma once
#include <cstdint>

// Startup only: verified build-35924 CreateBuildingAssets inline MT selection.
// Windows modulo/rejection and singleton draw behavior; original MT retained.
bool Tpf2mpInstallTreeRng(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpTreeRngStatus();
