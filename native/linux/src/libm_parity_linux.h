#pragma once
#include <cstdint>

// Startup only (before game main): make the verified build-35924 native game
// compute its float trigonometry exactly like the Windows build (ucrtbase.dll,
// AVX2+FMA paths). Kill switch: TPF2MP_LIBM_PARITY=0.
//   - the game image's sinf, cosf, sincosf, tanf, acosf and atan2f imports
//     (.got.plt, bound at load) are pointed at windows_ucrt_math_linux.cpp;
//   - three street developer call sites where GCC called the DOUBLE atan2 on
//     float arguments (MSVC calls atan2f there) are redirected to the UCRT
//     atan2f model through a near stub.
// Every slot and site is verified (build id, slot value resolves to libm's
// function of that name, guard bytes around each call) before anything is
// written; a failure rolls back what was written and leaves the module off.
bool Tpf2mpInstallLibmParity(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpLibmParityStatus();

// The call-site replacement: a double-in/double-out atan2 that rounds its
// (float-valued) arguments back to float and returns the UCRT atan2f result.
extern "C" double Tpf2mpUcrtAtan2FromFloats(double y, double x);
