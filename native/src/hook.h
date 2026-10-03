// Minimal x64 inline hook: 14/15/17-byte absolute-jump detour + trampoline.
// Self-contained, no external dependencies (no MinHook).
//
// Patch layout at target (15 or 17 bytes, per M1 prologue analysis):
//   FF 25 00 00 00 00  <8-byte absolute detour address>   -> jmp [rip+0]
//   CC ... (padding over remaining stolen bytes)
// Trampoline (allocated with VirtualAlloc):
//   <stolen bytes, verbatim>  FF 25 00 00 00 00 <target+stealBytes>
//
// Constraint: stolen bytes must not contain RIP-relative instructions.
// Verified statically for both M2 hook targets (see m1/prologue dumps).
#pragma once
#include <cstdint>

bool InstallHook(uintptr_t target, void* detour, int stealBytes, void** trampolineOut);

// How many bytes to steal at `code` so that the cut lands on an instruction
// boundary at or after `minBytes` (14 = the absolute jump InstallHook
// writes). Decodes only the plain, position-independent instructions a
// function prologue is made of (push/pop, mov/lea/sub/add/xor/test with a
// ModRM operand, small immediates, nops). Returns 0 when it meets anything
// else -- a call, a jump, a RIP-relative operand -- because those cannot be
// copied into a trampoline unchanged. Never guess a steal for a DLL we do
// not ship: vulkan-1.dll comes with the graphics driver, and a fixed 15
// split an instruction on a friend's loader (2026-09-10).
int PrologueSteal(const unsigned char* code, int minBytes);

// The same, but a RIP-relative memory operand is accepted: the offset (from
// `code`) of each such instruction's disp32 goes to `fixups` (at most
// `maxFixups`; their count to *nFixups). InstallHookRip copies the prologue
// into a trampoline allocated within +-2 GB of the target and re-aims those
// displacements at the same addresses. For a DLL whose prologue we do not
// control: Wine's vkGetDeviceProcAddr opens with test byte [rip+x],8, and the
// overlay was never hooked under CrossOver on macOS (2026-10-03).
int PrologueStealRip(const unsigned char* code, int minBytes, int* fixups, int maxFixups, int* nFixups);
bool InstallHookRip(uintptr_t target, void* detour, int stealBytes, const int* fixups, int nFixups,
                    void** trampolineOut);
