// ---------------------------------------------------------------------------
// defer_hook — intercept a player's build BEFORE the engine acts on it.
//
// This is the first DESTRUCTIVE hook in this project. Everything until now has
// observed and let the original run. Suppression is therefore OFF unless the
// config file explicitly turns it on.
//
// TARGET: StreetBuilder::UpdateEngine, RVA 0x459ce0. Verified live on build
// 35924 (docs/re/UI_CAPTURE_PATH.md):
//   243 engine-internal applyProposal commits (towns building) -> 0 hits here
//   dragging a preview                                          -> 0 hits
//   cancelling with X                                           -> 0 hits
//   ONE completed player build                                  -> exactly 1 hit
// applyProposal itself is useless for this: it is the shared downstream of the
// UI and the engine's own construction, so suppressing there would stop towns
// growing.
//
// WHAT THIS VERSION DOES AND DOES NOT DO
// It proves the CANCEL half of deferral. It does not yet re-issue the command:
// extracting the proposal geometry out of the builder object is a separate
// problem. So with suppression enabled a player's build simply does not happen
// -- that is the mechanism test, not a usable feature, and exactly why the flag
// defaults off.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include "hook.h"

// Prologue boundaries are 3,4,5,7,14,21,30 with no RIP-relative bytes, and 21 is
// a size hook.cpp supports. The function opens `mov rax, rsp`, which the
// trampoline re-executes -- safe only because the relay restores rsp to its
// entry value before jumping there.
static const uintptr_t TARGET_RVA = 0x459ce0;
static const int TARGET_STEAL = 21;
static const int BLOB_SIZE = 48;

extern "C" void DeferRelay();

static FILE* g_log = nullptr;
static bool  g_suppress = false;
static long  g_seen = 0, g_suppressed = 0;
static uintptr_t g_base = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, g_log);
    fflush(g_log);
}

static const char* OUT_DIR =
    "C:/Program Files (x86)/Steam/steamapps/workshop/content/1066780/3710243057/recon/m4/out/";

// Read the flag from a file rather than baking it in: flipping suppression must
// not require a rebuild, because a DLL cannot be replaced without restarting the
// game (a lesson that cost three measurement cycles here).
static bool ReadSuppressFlag()
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%stpf2_defer.cfg", OUT_DIR);
    FILE* f = _fsopen(p, "r", _SH_DENYNO);
    if (!f) return false;
    char line[128] = { 0 };
    bool on = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "suppress=1")) on = true;
    }
    fclose(f);
    return on;
}

// The intent record. Only what this hook can actually see so far: that a build
// happened, and the builder instance it came from. Deliberately NOT pretending
// to carry geometry -- the proposal is still locked inside the builder object.
static void WriteIntent(uint64_t builderThis, uint64_t retAddr)
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%stpf2_intent.txt", OUT_DIR);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) return;
    fprintf(f, "BUILD_INTENT t=%llu builder=%llx caller_rva=%llx suppressed=%d\n",
        (unsigned long long)GetTickCount64(),
        (unsigned long long)builderThis,
        (unsigned long long)(retAddr - g_base),
        g_suppress ? 1 : 0);
    fclose(f);
}

// rax: 0 = let the original run, 1 = cancel it
extern "C" uint64_t DeferHandler(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9,
                                 uint64_t id, uint64_t retAddr)
{
    (void)rdx; (void)r8; (void)r9; (void)id;
    g_seen++;
    // Re-read per event so the flag can be flipped while the game runs.
    g_suppress = ReadSuppressFlag();

    __try {
        WriteIntent(rcx, retAddr);
        if (g_suppress) {
            g_suppressed++;
            Log("[defer] #%ld SUPPRESSED builder=%llx caller_rva=%llx"
                " -- the build did NOT happen locally\n",
                g_seen, (unsigned long long)rcx,
                (unsigned long long)(retAddr - g_base));
            return 1;
        }
        Log("[defer] #%ld observed builder=%llx caller_rva=%llx (original ran)\n",
            g_seen, (unsigned long long)rcx,
            (unsigned long long)(retAddr - g_base));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[defer] handler fault -- proceeding, never cancel on an error\n");
        return 0;
    }
    return 0;
}

static void WriteBlob(uint8_t* p, int id, void* relay)
{
    int o = 0;
    p[o++] = 0x41; p[o++] = 0x52;                       // push r10
    p[o++] = 0x50;                                      // push rax
    p[o++] = 0xB8; memcpy(p + o, &id, 4); o += 4;       // mov eax, id
    p[o++] = 0x49; p[o++] = 0xBA; o += 8;               // mov r10, tramp (patched)
    p[o++] = 0x41; p[o++] = 0x52;                       // push r10
    p[o++] = 0x49; p[o++] = 0xBA;                       // mov r10, relay
    memcpy(p + o, &relay, 8); o += 8;
    p[o++] = 0x41; p[o++] = 0xFF; p[o++] = 0xE2;        // jmp r10
}

static DWORD WINAPI Init(LPVOID)
{
    // Refuse a second copy. Injecting one twice does not fail loudly: the newer
    // DLL patches an address the older already detoured and their log handles
    // clobber each other, so the output looks like the hook never fired.
    HANDLE once = CreateMutexA(nullptr, TRUE, "tpf2_defer_hook_single_instance");
    if (once == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%stpf2_defer.log", OUT_DIR);
    g_log = _fsopen(path, "w", _SH_DENYWR);
    if (!g_log) return 0;

    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    g_suppress = ReadSuppressFlag();
    Log("[defer] attached, base=%llx\n", (unsigned long long)g_base);
    Log("[defer] suppression is %s (tpf2_defer.cfg -> suppress=1 to enable)\n",
        g_suppress ? "ON -- player builds will be CANCELLED" : "off (observe only)");

    uint8_t* blob = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blob) { Log("[defer] blob alloc failed\n"); return 0; }
    WriteBlob(blob, 0, (void*)&DeferRelay);
    void* tramp = nullptr;
    if (InstallHook(g_base + TARGET_RVA, blob, TARGET_STEAL, &tramp)) {
        memcpy(blob + 10, &tramp, 8);
        FlushInstructionCache(GetCurrentProcess(), blob, BLOB_SIZE);
        Log("[defer] hooked StreetBuilder::UpdateEngine rva=%llx\n",
            (unsigned long long)TARGET_RVA);
    } else {
        Log("[defer] HOOK FAILED rva=%llx -- prologue changed on this build?\n",
            (unsigned long long)TARGET_RVA);
        return 0;
    }

    for (;;) {
        Sleep(15000);
        Log("[defer] alive: seen=%ld suppressed=%ld suppress_flag=%d\n",
            g_seen, g_suppressed, g_suppress ? 1 : 0);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
