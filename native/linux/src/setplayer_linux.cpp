// Linux build of setplayer_patch.cpp: game.interface.setPlayer for every
// entity kind. What the binding does, and why its assert has to go, is
// commented in setplayer_patch.cpp; this file has the Linux sites.
//
// THE BINDING, Linux build 35924: scripting::SetupInterface's setPlayer lambda
// at 0x1dc4ed0. The same three lookups as on Windows -- Construction ->
// SetPlayerForConstruction (0x1dc4c70); Line -> its stops, edges and vehicles;
// AssetGroup -> the generic owner setter 0x1dc4860(engine, &entity, &player),
// the call the Line branch also makes for each vehicle and edge -- and the same
// assert(false) at interface.cpp:2340 (0x1dc5840) when none is found.
//
// GCC reaches that assert from TWO places, where MSVC had one:
//   A  0x1dc51d4  cmp  qword [rbp-1c0h], 0   ; the AssetGroup component, or null
//      0x1dc51dc  je   0x1dc5840             ; null -> assert
//      0x1dc51e2  lea  rdx, [rbp-1ach]       ; &player
//      0x1dc51e9  mov  rsi, r13              ; &entity
//      0x1dc51ec  mov  rdi, rbx              ; the engine
//      0x1dc51ef  call 0x1dc4860             ; the generic owner setter
//      0x1dc51f4  jmp  0x1dc5245             ; the common exit
//      0x1dc51f6  10 bytes of alignment padding
//   B  0x1dc5290  test r14, r14              ; the Line component, or null
//      0x1dc5293  je   0x1dc5840             ; (the entity has no AssetGroup at all)
//
// THE PATCH.
//   A: the je becomes a 6-byte nop, so a null AssetGroup falls into the setter.
//   B: r13 is not always &entity on the paths into B -- a reaching-definitions
//      pass over the function finds the value it was entered with on one of
//      them -- so B cannot jump straight to 0x1dc51e2. The padding at 0x1dc51f6
//      becomes
//        lea r13, [rbp-1b0h]      ; &entity
//        jmp short 0x1dc51e2
//      and B's je points at it.
// Checked over the function's control flow before patching (2026-09-12): no
// indirect jumps and nothing branching or pointing into the padding; the
// entity slot [rbp-1b0h], the player slot [rbp-1ach] and the object the common
// exit destroys ([rbp-1e8h]) are written on every path to both sites; rbx is
// the engine at both.
#include "setplayer_patch.h"
#include "game_image.h"
#include "codewrite_linux.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>

static const uintptr_t RVA_A      = 0x1dc51d4;
static const uintptr_t RVA_B      = 0x1dc5290;
static const uintptr_t RVA_ASSERT = 0x1dc5840;

static const uint8_t EXPECTED_A[44] = {
    0x48, 0x83, 0xbd, 0x40, 0xfe, 0xff, 0xff, 0x00,              // cmp  qword [rbp-1c0h], 0
    0x0f, 0x84, 0x5e, 0x06, 0x00, 0x00,                          // je   0x1dc5840
    0x48, 0x8d, 0x95, 0x54, 0xfe, 0xff, 0xff,                    // lea  rdx, [rbp-1ach]
    0x4c, 0x89, 0xee,                                            // mov  rsi, r13
    0x48, 0x89, 0xdf,                                            // mov  rdi, rbx
    0xe8, 0x6c, 0xf6, 0xff, 0xff,                                // call 0x1dc4860
    0xeb, 0x4f,                                                  // jmp  0x1dc5245
    0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,  // padding
};
static const uint8_t EXPECTED_B[9] = {
    0x4d, 0x85, 0xf6,                                            // test r14, r14
    0x0f, 0x84, 0xa7, 0x05, 0x00, 0x00,                          // je   0x1dc5840
};
// mov edx, 924h -- line 2340 -- right after the assert block's first lea: the
// block both jumps leave is really interface.cpp:2340.
static const uint8_t EXPECTED_ASSERT_LINE[5] = { 0xba, 0x24, 0x09, 0x00, 0x00 };

static const size_t  A_JE = 8, A_CAVE = 34, B_JE = 3;
static const uint8_t NOP6[6] = { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 };
static const uint8_t CAVE[10] = {
    0x4c, 0x8d, 0xad, 0x50, 0xfe, 0xff, 0xff,                    // lea  r13, [rbp-1b0h]
    0xeb, 0xe3,                                                  // jmp  0x1dc51e2
    0x90,
};
static const uint8_t JE_TO_CAVE[6] = { 0x0f, 0x84, 0x5d, 0xff, 0xff, 0xff };   // je 0x1dc51f6

bool SetPlayerPatch_Install(SetPlayerLogFn log)
{
    const Tpf2GameImage img = Tpf2mpGameImage();
    if (!img.buildOk) {
        log("[setplayer] the game is not build 35924 (GNU build-id differs) -- not patched\n");
        return false;
    }
    const uintptr_t a = img.base + RVA_A;
    const uintptr_t b = img.base + RVA_B;

    uint8_t doneA[sizeof(EXPECTED_A)], doneB[sizeof(EXPECTED_B)];
    memcpy(doneA, EXPECTED_A, sizeof(doneA));
    memcpy(doneA + A_JE, NOP6, sizeof(NOP6));
    memcpy(doneA + A_CAVE, CAVE, sizeof(CAVE));
    memcpy(doneB, EXPECTED_B, sizeof(doneB));
    memcpy(doneB + B_JE, JE_TO_CAVE, sizeof(JE_TO_CAVE));
    if (memcmp((void*)a, doneA, sizeof(doneA)) == 0 && memcmp((void*)b, doneB, sizeof(doneB)) == 0) {
        log("[setplayer] already patched\n");
        return true;
    }
    if (memcmp((void*)a, EXPECTED_A, sizeof(EXPECTED_A)) != 0 ||
        memcmp((void*)b, EXPECTED_B, sizeof(EXPECTED_B)) != 0 ||
        memcmp((void*)(img.base + RVA_ASSERT + 7), EXPECTED_ASSERT_LINE, sizeof(EXPECTED_ASSERT_LINE)) != 0) {
        log("[setplayer] the interface.cpp:2340 branches differ from build 35924 -- not patched: "
            "setPlayer on a track, road, node, signal or line-less vehicle still asserts "
            "and leaves the owner unchanged\n");
        return false;
    }

    // The cave first (nothing reaches it yet), then B, which starts using it, then A.
    int err = 0;
    if (Tpf2mpCodeWriteSelf(a + A_CAVE, CAVE, sizeof(CAVE), &err) != TPF2MP_CW_OK ||
        Tpf2mpCodeWriteSelf(b + B_JE, JE_TO_CAVE, sizeof(JE_TO_CAVE), &err) != TPF2MP_CW_OK ||
        Tpf2mpCodeWriteSelf(a + A_JE, NOP6, sizeof(NOP6), &err) != TPF2MP_CW_OK) {
        log("[setplayer] code write failed (%s) -- patch incomplete; initialized cave retained\n", strerror(err));
        return false;
    }
    log("[setplayer] patched %lx and %lx: setPlayer re-owns any entity through the engine's owner setter "
        "(no interface.cpp:2340 assert)\n", (unsigned long)(RVA_A + A_JE), (unsigned long)(RVA_B + B_JE));
    return true;
}
