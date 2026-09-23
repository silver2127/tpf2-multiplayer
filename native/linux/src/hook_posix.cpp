// POSIX build of hook.h: the same 14-byte absolute-jump detour and trampoline
// as hook.cpp, with mmap/mprotect for VirtualAlloc/VirtualProtect.
//
// The prologue decoder is hook.cpp's, plus ENDBR64 (f3 0f 1e fa): the Linux
// build is compiled with -fcf-protection, so every function starts with one.
// It is plain position-independent code and copies into a trampoline as is.
#include "hook.h"
#include "codewrite_linux.h"
#include "near_alloc.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

static const uint8_t JMP_ABS_PREFIX[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };

bool InstallHook(uintptr_t target, void* detour, int stealBytes, void** trampolineOut)
{
    if (!target || !detour || !trampolineOut || stealBytes < 5 || stealBytes > 32) return false;
    const bool nearJump = stealBytes < 14;

    // 1. trampoline: stolen bytes + abs jump back to target+stealBytes.
    // Written while RW, then flipped to RX: never writable and executable at once.
    void* mem = nearJump ? Tpf2mpAllocNear(target)
                        : mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!mem || mem == MAP_FAILED) return false;
    uint8_t* tramp = (uint8_t*)mem;
    memcpy(tramp, (void*)target, stealBytes);
    memcpy(tramp + stealBytes, JMP_ABS_PREFIX, 6);
    uintptr_t backAddr = target + stealBytes;
    memcpy(tramp + stealBytes + 6, &backAddr, 8);
    const uintptr_t det = (uintptr_t)detour;
    if (nearJump) {
        memcpy(tramp + 128, JMP_ABS_PREFIX, 6);
        memcpy(tramp + 134, &det, 8);
    }
    if (mprotect(mem, 4096, PROT_READ | PROT_EXEC) != 0) { munmap(mem, 4096); return false; }

    // Published before the target is patched: a game thread can enter the
    // detour the instant the jump is written, and the detour calls through
    // this pointer. (hook.cpp assigns it afterwards.)
    *trampolineOut = tramp;

    // 2. Patch without changing game page permissions. Every library uses this
    // path: one writer cannot restore RX while another is still writing.
    uint8_t patch[32];
    if (nearJump) {
        const intptr_t offset = (intptr_t)(tramp + 128) - (intptr_t)(target + 5);
        if (offset < INT32_MIN || offset > INT32_MAX) {
            *trampolineOut = nullptr;
            munmap(mem, 4096);
            return false;
        }
        const int32_t rel = (int32_t)offset;
        patch[0] = 0xe9;
        memcpy(patch + 1, &rel, sizeof(rel));
        memset(patch + 5, 0xcc, stealBytes - 5);
    } else {
        memcpy(patch, JMP_ABS_PREFIX, 6);
        memcpy(patch + 6, &det, 8);
        memset(patch + 14, 0xCC, stealBytes - 14);
    }
    int err = 0;
    const int result = Tpf2mpCodeWriteSelf(target, patch, stealBytes, &err);
    if (result == TPF2MP_CW_OK) return true;
    // A possibly visible jump must retain its trampoline. Only an untouched
    // target permits unmapping it; restored targets may already have entrants.
    if (result == TPF2MP_CW_UNAVAILABLE) {
        *trampolineOut = nullptr;
        munmap(mem, 4096);
    }
    return false;
}

// ---- prologue length decoder (see hook.h) ----------------------------------
// One instruction's length, or 0 if it is not one of the safe kinds.
static int SafeInsnLen(const unsigned char* p)
{
    // endbr64
    if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA) return 4;
    int i = 0;
    const bool osz = p[i] == 0x66;
    if (osz) i++;
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
            return modrm(osz && !rexW ? 2 : 4);
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            return i + (rexW ? 8 : osz ? 2 : 4);            // mov r, imm
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
