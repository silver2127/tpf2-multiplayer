#include "simhook.h"
#include "hook.h"
#include <windows.h>
#include <cstring>
#include <cstdio>

// GameSim::Step(__int64 frameTime, int) -- RVA, so ASLR-safe.
// Located via CGame::RunGameSimLoop, confirmed by its own assertion
// (cmp rdx, 0x3e8 / jl -> the "frameTime >= 1000" stub). See M7 doc.
static const uintptr_t STEP_RVA = 0x15aa00;
static const int       STEAL    = 21;

// The exact prologue we reversed. Verified: 21 lands on an instruction
// boundary and none of these bytes are rip-relative, so they relocate verbatim
// into the trampoline. If a game patch changes them we refuse to install --
// blindly writing a jmp into a shifted function would corrupt something random.
//
//   40 53                 push rbx        (note the redundant 0x40 REX prefix)
//   41 56                 push r14
//   48 83 EC 68           sub  rsp, 68h
//   48 8B DA              mov  rbx, rdx
//   4C 8B F1              mov  r14, rcx
//   48 81 FA E8 03 00 00  cmp  rdx, 3E8h
static const uint8_t EXPECTED[STEAL] = {
    0x40, 0x53, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x68,
    0x48, 0x8B, 0xDA, 0x4C, 0x8B, 0xF1, 0x48, 0x81,
    0xFA, 0xE8, 0x03, 0x00, 0x00,
};

extern "C" {
    // consumed by simsteprelay.asm
    void* g_simStepTramp = nullptr;
    void  SimStepRelay();

    // Runs on the Simulation Thread, once per sim step. Keep it to plain stores:
    // no allocation, no locks, no file I/O. M2's always-on logging hook wrote
    // ~56 GB of noise in one session; a per-tick hook is exactly where that
    // mistake repeats. Reporting is done elsewhere, off this thread.
    volatile uint64_t g_simTicks = 0;
    volatile uint64_t g_simLastFrameTime = 0;
    volatile void*    g_simThis = nullptr;

    void SimStepHandler(void* gameSim, uint64_t frameTime)
    {
        g_simTicks = g_simTicks + 1;
        g_simLastFrameTime = frameTime;
        g_simThis = gameSim;
    }
}

uint64_t SimHook_TickCount()     { return g_simTicks; }
uint64_t SimHook_LastFrameTime() { return g_simLastFrameTime; }

// Is [addr, addr+len) inside this module's mapped image?
static bool RangeInModule(HMODULE mod, uintptr_t addr, size_t len)
{
    auto dos = (const IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    uintptr_t base = (uintptr_t)mod;
    return addr >= base && (addr + len) <= base + nt->OptionalHeader.SizeOfImage;
}

static bool Readable(uintptr_t addr, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD)) return false;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return addr + len <= regionEnd;
}

bool SimHook_Install(SimLogFn log)
{
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) {
        if (log) log("[simhook] GetModuleHandle failed\n");
        return false;
    }

    // The bridge also gets loaded by test harnesses and by whatever else might
    // pull it in. Patching module-base + a Transport Fever RVA in some other
    // executable would be, at best, an access violation -- and the first
    // version of this did exactly that and killed its own thread silently.
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(base, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    if (_wcsicmp(leaf, L"TransportFever2.exe") != 0) {
        if (log) log("[simhook] host is '%S', not TransportFever2.exe -- skipping\n", leaf);
        return false;
    }
    if (!RangeInModule(base, (uintptr_t)base + STEP_RVA, STEAL)) {
        if (log) log("[simhook] RVA 0x%llx is outside the image -- skipping\n",
                     (unsigned long long)STEP_RVA);
        return false;
    }
    return SimHook_InstallAt((uintptr_t)base + STEP_RVA, log);
}

bool SimHook_InstallAt(uintptr_t target, SimLogFn log)
{
    if (!Readable(target, STEAL)) {
        if (log) log("[simhook] target %p not readable -- skipping\n", (void*)target);
        return false;
    }
    if (memcmp((const void*)target, EXPECTED, STEAL) != 0) {
        if (log) {
            const uint8_t* got = (const uint8_t*)target;
            char buf[128];
            int n = 0;
            for (int i = 0; i < 8 && n < 100; ++i)
                n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "%02X ", got[i]);
            log("[simhook] REFUSING to patch: prologue mismatch at %p\n"
                "[simhook]   expected 40 53 41 56 48 83 EC 68 ...\n"
                "[simhook]   found    %s...\n"
                "[simhook]   (game updated? re-run tools/tpfdis.py on GameSim::Step)\n",
                (void*)target, buf);
        }
        return false;
    }

    void* tramp = nullptr;
    if (!InstallHook(target, (void*)&SimStepRelay, STEAL, &tramp)) {
        if (log) log("[simhook] InstallHook failed\n");
        return false;
    }
    g_simStepTramp = tramp;

    if (log) log("[simhook] hooked GameSim::Step at %p (tramp %p)\n",
                 (void*)target, tramp);
    return true;
}
