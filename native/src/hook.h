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
