#include "hook.h"
#include <windows.h>
#include <cstring>

static const uint8_t JMP_ABS_PREFIX[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };

bool InstallHook(uintptr_t target, void* detour, int stealBytes, void** trampolineOut)
{
    if (stealBytes < 14) return false;

    // 1. trampoline: stolen bytes + abs jump back to target+stealBytes
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, (void*)target, stealBytes);
    memcpy(tramp + stealBytes, JMP_ABS_PREFIX, 6);
    uintptr_t backAddr = target + stealBytes;
    memcpy(tramp + stealBytes + 6, &backAddr, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, stealBytes + 14);

    // 2. patch target: abs jump to detour, pad rest with int3
    DWORD oldProt;
    if (!VirtualProtect((void*)target, stealBytes, PAGE_EXECUTE_READWRITE, &oldProt))
    { VirtualFree(tramp, 0, MEM_RELEASE); return false; }

    uint8_t patch[32];
    memcpy(patch, JMP_ABS_PREFIX, 6);
    uintptr_t det = (uintptr_t)detour;
    memcpy(patch + 6, &det, 8);
    memset(patch + 14, 0xCC, stealBytes - 14);
    memcpy((void*)target, patch, stealBytes);

    VirtualProtect((void*)target, stealBytes, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, stealBytes);

    *trampolineOut = tramp;
    return true;
}
