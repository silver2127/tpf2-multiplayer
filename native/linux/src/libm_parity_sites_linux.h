#pragma once
// Verified sites for libm_parity_linux.cpp, native build 35924
// (build-id 3a0e156390b0e6f1e372051c24802c8493ae454a).
#include <cstddef>
#include <cstdint>

// .got.plt slots (R_X86_64_JUMP_SLOT r_offset). The image is linked with
// DF_BIND_NOW, so every slot holds the resolved libm address when boot runs;
// the slots lie in PT_GNU_RELRO (0x59a8700 + 0x9f900) and are written through
// Tpf2mpCodeWriteSelf.
struct Tpf2mpGotSlot { uintptr_t rva; const char* name; };
constexpr Tpf2mpGotSlot kLibmGotSinf{0x5a471b8, "sinf"};
constexpr Tpf2mpGotSlot kLibmGotCosf{0x5a471c8, "cosf"};
constexpr Tpf2mpGotSlot kLibmGotSincosf{0x5a470c0, "sincosf"};
constexpr Tpf2mpGotSlot kLibmGotTanf{0x5a470d8, "tanf"};
constexpr Tpf2mpGotSlot kLibmGotAcosf{0x5a472b8, "acosf"};
constexpr Tpf2mpGotSlot kLibmGotAtan2f{0x5a472b0, "atan2f"};

// PLT stub of the DOUBLE atan2 (jmp [rip -> 0x5a46cb0]).
constexpr uintptr_t kLibmPltAtan2Rva = 0x6dbc60;

// Call sites where GCC resolved ::atan2(float, float) to the double function
// but MSVC calls atan2f (the Windows functions import atan2f there):
//   0x1578b3b  CreateAngle2SegMap        Windows 0x140985d53 (in 0x140985ba0)
//   0x1579031  CheckBranchesRec          Windows 0x1409859d4 (in 0x140985800)
//   0x157a4ac  CheckBranches (inlined)   Windows atan2f in 0x140985310
// Each guard is the 8 bytes of float->double conversions before the call,
// the call itself, and the double->float conversion of the result after it.
struct Tpf2mpAtan2Site {
    uintptr_t callRva;
    uintptr_t guardRva;                 // first guarded byte
    unsigned char guard[35];
    size_t guardSize;
};
constexpr Tpf2mpAtan2Site kLibmAtan2Sites[] = {
    // 1578b33 cvtss2sd xmm0,xmm0 ; cvtss2sd xmm1,xmm1 ; call atan2 ; pxor xmm2,xmm2 ; mov edi,0x28 ; cvtsd2ss xmm2,xmm0
    {0x1578b3b, 0x1578b33,
     {0xf3,0x0f,0x5a,0xc0, 0xf3,0x0f,0x5a,0xc9, 0xe8,0x20,0x31,0x16,0xff,
      0x66,0x0f,0xef,0xd2, 0xbf,0x28,0x00,0x00,0x00, 0xf2,0x0f,0x5a,0xd0}, 26},
    // 1579029 cvtss2sd xmm0,xmm0 ; cvtss2sd xmm1,xmm1 ; call atan2 ; mov rax,[rbp-0x90] ; cvtsd2ss xmm0,xmm0
    {0x1579031, 0x1579029,
     {0xf3,0x0f,0x5a,0xc0, 0xf3,0x0f,0x5a,0xc9, 0xe8,0x2a,0x2c,0x16,0xff,
      0x48,0x8b,0x85,0x70,0xff,0xff,0xff, 0xf2,0x0f,0x5a,0xc0}, 24},
    // 157a4a4 cvtss2sd xmm1,xmm1 ; cvtss2sd xmm0,xmm0 ; call atan2 ; mov rsi,[rbp-0x1a58] ; (mov rax,.. ; pxor ; cvtsd2ss xmm1,xmm0)
    {0x157a4ac, 0x157a4a4,
     {0xf3,0x0f,0x5a,0xc9, 0xf3,0x0f,0x5a,0xc0, 0xe8,0xaf,0x17,0x16,0xff,
      0x48,0x8b,0xb5,0xa8,0xe5,0xff,0xff,
      0x48,0x8b,0x85,0xd8,0xe8,0xff,0xff, 0x66,0x0f,0xef,0xc9, 0xf2,0x0f,0x5a,0xc8}, 35},
};
constexpr size_t kLibmAtan2SiteCount = sizeof(kLibmAtan2Sites) / sizeof(kLibmAtan2Sites[0]);
