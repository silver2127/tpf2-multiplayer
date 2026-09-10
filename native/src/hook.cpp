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

// ---- prologue length decoder (see hook.h) ----------------------------------
// One instruction's length, or 0 if it is not one of the safe kinds.
static int SafeInsnLen(const unsigned char* p)
{
    int i = 0;
    // legacy prefixes seen in prologues (66 operand-size) and a REX byte
    if (p[i] == 0x66) i++;
    bool rexW = false;
    if ((p[i] & 0xF0) == 0x40) { rexW = (p[i] & 8) != 0; i++; }
    unsigned char op = p[i++];
    auto modrm = [&](int immAfter) -> int {
        unsigned char m = p[i++];
        int mod = m >> 6, rm = m & 7;
        if (mod == 3) return i + immAfter;
        if (rm == 4) i++;                                   // SIB
        if (mod == 0 && rm == 5) return 0;                  // RIP-relative: not copyable
        if (mod == 0 && rm == 4 && (p[i - 1] & 7) == 5) i += 4;   // SIB with disp32, no base
        if (mod == 1) i += 1;
        else if (mod == 2) i += 4;
        return i + immAfter;
    };
    if (op >= 0x50 && op <= 0x5F) return i;                 // push/pop r64
    if (op == 0x90 || op == 0xC3 || op == 0xCC) return i;   // nop, ret, int3
    switch (op) {
        case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23:
        case 0x29: case 0x2B: case 0x31: case 0x33: case 0x39: case 0x3B:
        case 0x84: case 0x85: case 0x87: case 0x88: case 0x89: case 0x8A:
        case 0x8B: case 0x8D: case 0x63:
            return modrm(0);
        case 0x80: case 0x83: case 0xC6:
            return modrm(1);
        case 0x81: case 0xC7:
            return modrm(4);
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            return i + (rexW ? 8 : 4);                      // mov r, imm
        case 0x0F: {
            unsigned char op2 = p[i++];
            if (op2 == 0x1F || op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF ||
                op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 || op2 == 0x57 || op2 == 0xEF)
                return modrm(0);
            return 0;
        }
        default:
            return 0;
    }
}

int PrologueSteal(const unsigned char* code, int minBytes)
{
    int n = 0;
    while (n < minBytes) {
        int l = SafeInsnLen(code + n);
        if (l <= 0 || n + l > 64) return 0;
        n += l;
    }
    return n;
}
