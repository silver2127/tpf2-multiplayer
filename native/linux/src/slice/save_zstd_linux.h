// save_zstd_linux.h -- the Linux installer of the shared save stream code
// (native/src/save_zstd.h): the system libzstd, dlopen'd.
#pragma once
#include "save_zstd.h"

// Installs the six call-site redirects (slice_core_main.cpp). Guard/config/API
// refusal changes nothing. Partial redirection leaves compression with the
// embedded API; takeover starts only after all six redirects succeed.
bool SliceInstallSaveZstd(uintptr_t base, const char* root, const char* data);
void SliceSaveZstdLogAlive();
