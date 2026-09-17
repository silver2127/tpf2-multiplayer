// The company wash on the HUD station/depot icons, for the Linux build.
#pragma once
#include <cstdint>

// Installs the icon tint. Call only after the build-id, slice read backend and
// no-foreign-patch gates pass. Returns false when a byte guard or a call
// redirect refused; the feature is then simply off and nothing is patched.
bool SliceInstallCompanyTint(uintptr_t base, const char* rootDir, const char* dataDir);

// Counters for the periodic "alive" line.
void SliceCompanyTintLogAlive();
