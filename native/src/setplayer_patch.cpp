#include "setplayer_patch.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

// Build 35924 (ImageBase 0x140000000), all RVAs.
//
// THE BINDING (0x1167090, decompiled and disassembled 2026-09-10).
// game.interface.setPlayer(entity, player) looks for three components on the
// entity and takes the first one it finds:
//   Construction -> 0x117df00, which also re-owns the construction's edges, its
//                   depots' vehicles, its stations and their station groups
//   Line         -> the line's stop stations, track edges and vehicles, then the line
//   AssetGroup   -> the generic owner setter 0x117dd20 on the entity
//   none         -> assert(false), interface.cpp:2340 (0x924)
// That assert is not a soft one. The call is followed by int3 (the compiler
// knows it cannot return): the handler 0x221adf0 prints "Assertion `false'
// failed.", writes a crash dump and throws, the binding hands the throw to Lua
// as an error, and the owner is never set. (Ghidra's decompile shows the setter
// after the assert; the machine code has no path there.) A company switch calls
// setPlayer on everything its player owns, so every loose track, road, node,
// signal, station and line-less vehicle wrote a dump -- 98 in a row froze a
// host, 2026-09-10 -- and stayed with the old company.
//
// THE PATCH. The end of the binding is
//   0x116779e  48 85 db              test rbx, rbx         ; the AssetGroup, or null
//   0x11677a1  74 42                 je   0x11677e5        ; null -> the assert block
//   0x11677a3  4c 8d 85 80 00 00 00  lea  r8,  [rbp+80h]   ; &player (Lua argument 2)
//   0x11677aa  48 8d 95 90 00 00 00  lea  rdx, [rbp+90h]   ; &entity (Lua argument 1)
//   0x11677b1  49 8b cf              mov  rcx, r15         ; the entity manager
//   0x11677b4  e8 67 65 01 00        call 0x117dd20        ; the generic owner setter
//   0x11677b9  90                    nop
//   0x11677ba  ...                   the common exit
// The setter reads neither rbx nor anything the AssetGroup lookup produced, and
// it is the same call the Line branch makes for each of its vehicles and track
// edges. The je becomes a 2-byte nop, so an entity with none of the three
// components goes to the setter too: no assert, no dump, and the owner changes.
static const uintptr_t RVA_TEST = 0x116779e;   // test rbx, rbx
static const uintptr_t RVA_JE   = 0x11677a1;   // je -> the assert block

// 0x116779e .. 0x1167805: the test, the setter call, the common exit and the
// assert block down to its call. Everything in it is rip-relative, so it is the
// same at any load address; a different game build is refused, never patched.
static const uint8_t EXPECTED[] = {
    0x48, 0x85, 0xdb,                               // test rbx, rbx
    0x74, 0x42,                                     // je   0x11677e5
    0x4c, 0x8d, 0x85, 0x80, 0x00, 0x00, 0x00,       // lea  r8, [rbp+80h]
    0x48, 0x8d, 0x95, 0x90, 0x00, 0x00, 0x00,       // lea  rdx, [rbp+90h]
    0x49, 0x8b, 0xcf,                               // mov  rcx, r15
    0xe8, 0x67, 0x65, 0x01, 0x00,                   // call 0x117dd20
    0x90,                                           // nop
    0x48, 0x8d, 0x4c, 0x24, 0x58,                   // lea  rcx, [rsp+58h]
    0xe8, 0x4c, 0xd6, 0x27, 0x01,                   // call 0x23e4e10
    0x48, 0x8b, 0x9c, 0x24, 0x88, 0x01, 0x00, 0x00, // mov  rbx, [rsp+188h]
    0x48, 0x81, 0xc4, 0x40, 0x01, 0x00, 0x00,       // add  rsp, 140h
    0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, // pop  r15, r14, r13, r12
    0x5f, 0x5e, 0x5d, 0xc3,                         // pop  rdi, rsi, rbp; ret
    0xe8, 0xe6, 0xe9, 0xa8, 0x01,                   // call 0x2bf61ca
    0x90,                                           // nop
    0x4c, 0x8d, 0x0d, 0x54, 0xfa, 0x15, 0x02,       // lea  r9, [the lambda's signature]
    0x41, 0xb8, 0x24, 0x09, 0x00, 0x00,             // mov  r8d, 924h (line 2340)
    0x48, 0x8d, 0x15, 0xd7, 0xea, 0x15, 0x02,       // lea  rdx, [interface.cpp's path]
    0x48, 0x8d, 0x0d, 0xb8, 0xa7, 0xda, 0x01,       // lea  rcx, ["false"]
    0xe8, 0xeb, 0x35, 0x0b, 0x01,                   // call 0x221adf0 (the assert)
};
static const uint8_t NOP2[2] = { 0x66, 0x90 };     // xchg ax, ax

bool SetPlayerPatch_Install(SetPlayerLogFn log)
{
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    const uint8_t* at = (const uint8_t*)(base + RVA_TEST);
    const size_t jeAt = RVA_JE - RVA_TEST;

    uint8_t done[sizeof(EXPECTED)];
    memcpy(done, EXPECTED, sizeof(EXPECTED));
    memcpy(done + jeAt, NOP2, sizeof(NOP2));
    if (memcmp(at, done, sizeof(done)) == 0) {
        log("[setplayer] already patched\n");
        return true;
    }
    if (memcmp(at, EXPECTED, sizeof(EXPECTED)) != 0) {
        log("[setplayer] the interface.cpp:2340 branch differs from build 35924 -- not patched: "
            "setPlayer on a track, road, node, signal or line-less vehicle still writes a crash dump "
            "and leaves the owner unchanged\n");
        return false;
    }

    void* je = (void*)(base + RVA_JE);
    DWORD old;
    if (!VirtualProtect(je, sizeof(NOP2), PAGE_EXECUTE_READWRITE, &old)) {
        log("[setplayer] VirtualProtect failed (%lu) -- not patched\n", GetLastError());
        return false;
    }
    memcpy(je, NOP2, sizeof(NOP2));
    VirtualProtect(je, sizeof(NOP2), old, &old);
    FlushInstructionCache(GetCurrentProcess(), je, sizeof(NOP2));
    log("[setplayer] patched %llx: setPlayer re-owns any entity through the engine's owner setter "
        "(no interface.cpp:2340 assert, no crash dump)\n", (unsigned long long)RVA_JE);
    return true;
}
