// ---------------------------------------------------------------------------
// payload_probe — read the Command payload's vectors, where the geometry lives.
//
// Hooks the wrapper FUN_1409dd6a0 (rva 0x9dd6a0), which receives the live
// 0xB18-byte payload in rdx and turns it into the 56-byte Command handle. That
// is the last point where the payload is addressable before it becomes opaque.
//
// Offsets come from decompiling the payload's copy constructor (0x9db920) with
// a prototype applied. Its 8-byte-spaced triplets are std::vector
// begin/end/capacity:
//     vector A  0x318 0x320 0x328
//     vector B  0x330 0x338 0x340
//     vector C  0xb00 0xb08 0xb10
//     type tag  0xb18   (the dispatch-table index; the wrapper reads exactly this)
//
// A vector gives its element count for free as (end - begin), so this reads real
// elements instead of guessing at structure -- which is the point. Two rounds of
// blind pointer-chasing from the Command handle found only allocator metadata,
// because the handle's own fields are container bookkeeping.
//
// Observe-only.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include "hook.h"

// Prologue: mov r11,rsp / mov [r11+0x10],rdx / mov [r11+8],rcx / 5 pushes /
// sub rsp,0xb80. Boundaries 3,7,11,12,13,15,17,19,26; 19 is a real boundary and
// a size hook.cpp supports, with no RIP-relative bytes. The homing stores use
// r11=rsp, so the trampoline needs rsp at its entry value -- which the relay
// restores before jumping there.
static const uintptr_t TARGET_RVA = 0x9dd6a0;
static const int TARGET_STEAL = 19;
static const int BLOB_SIZE = 48;

static const int MAX_DETAILED = 4;   // two builds is the experiment; a few spare

extern "C" void ApplyRelayProbe();

static FILE* g_log = nullptr;
static long g_hits = 0, g_detailed = 0;
static uintptr_t g_base = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[900];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, g_log);
    fflush(g_log);
}

static bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// Read a std::vector triplet and dump its ELEMENTS.
static void DumpVector(const char* name, uint64_t payload, unsigned offBegin)
{
    uint64_t begin = 0, end = 0;
    if (!Readable((void*)(payload + offBegin), 16)) { Log("[pl]   %s unreadable\n", name); return; }
    memcpy(&begin, (void*)(payload + offBegin), 8);
    memcpy(&end,   (void*)(payload + offBegin + 8), 8);
    if (begin == 0 && end == 0) { Log("[pl]   %s +%03x EMPTY\n", name, offBegin); return; }
    if (end < begin || end - begin > 0x40000) {
        Log("[pl]   %s +%03x begin=%llx end=%llx (not a plausible vector)\n",
            name, offBegin, (unsigned long long)begin, (unsigned long long)end);
        return;
    }
    uint64_t bytes = end - begin;
    Log("[pl]   %s +%03x begin=%llx end=%llx bytes=%llu\n",
        name, offBegin, (unsigned long long)begin, (unsigned long long)end,
        (unsigned long long)bytes);
    size_t n = (size_t)(bytes > 0x100 ? 0x100 : bytes);
    if (!Readable((void*)begin, n)) { Log("[pl]     elements unreadable\n"); return; }
    const uint8_t* b = (const uint8_t*)begin;
    char line[200];
    for (size_t row = 0; row * 16 < n; row++) {
        int o = snprintf(line, sizeof(line), "[pl]     +%03x: ", (unsigned)(row * 16));
        for (int c = 0; c < 16; c++)
            o += snprintf(line + o, sizeof(line) - o, "%02x", b[row * 16 + c]);
        line[o] = 0;
        Log("%s\n", line);
    }
    // World coordinates are the thing we are hunting; report every float that
    // could be one rather than pre-judging the scale (a filter that excluded
    // small values has already hidden a result once tonight).
    for (size_t off = 0; off + 4 <= n; off += 4) {
        float f; memcpy(&f, b + off, 4);
        float a = f < 0 ? -f : f;
        if (f == f && a > 0.01f && a < 1000000.0f)
            Log("[pl]     +%03x FLOAT=%.3f\n", (unsigned)off, f);
    }
}

extern "C" void ApplyHandlerProbe(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9,
                                  uint64_t id, uint64_t retAddr)
{
    (void)r8; (void)r9; (void)id;
    long n = InterlockedIncrement(&g_hits);
    if (g_detailed >= MAX_DETAILED) return;
    __try {
        uint32_t type = 0xffffffff;
        if (Readable((void*)(rdx + 0xb18), 4)) memcpy(&type, (void*)(rdx + 0xb18), 4);
        g_detailed++;
        Log("\n[pl] ===== #%ld t=%llums caller_rva=%llx payload=%llx cmd_out=%llx =====\n",
            n, (unsigned long long)GetTickCount64(),
            (unsigned long long)(retAddr - g_base),
            (unsigned long long)rdx, (unsigned long long)rcx);
        (void)type;

        // WHOLESALE dump, no interpreted offsets.
        //
        // The previous version read "vectors" at 0x318/0x330/0xb00 and a type tag
        // at 0xb18, offsets taken from a function Ghidra decompiled WITHOUT a
        // recovered signature -- so its param_1/param_2 were the decompiler's
        // guesses, not a struct layout. Live probing returned end=0, begin==end
        // and a "type" of 0xED3BD08F. The offsets meant nothing.
        //
        // Dump the region instead and diff two builds made in different places.
        // Whatever changes IS the geometry, by construction, and that conclusion
        // survives being wrong about the layout. Same technique that proved the
        // 56-byte Command is a handle (12 of 13 fields identical across builds).
        //
        // 0xB40 = the 0xB18 stack local, plus a little to catch a trailing tag.
        const size_t N = 0xB40;
        if (!Readable((void*)rdx, N)) {
            Log("[pl]   payload not fully readable at %llx\n", (unsigned long long)rdx);
        } else {
            const uint8_t* b = (const uint8_t*)rdx;
            char line[160];
            for (size_t row = 0; row * 16 < N; row++) {
                int o = snprintf(line, sizeof(line), "[pl] %04x: ", (unsigned)(row * 16));
                for (int c = 0; c < 16; c++)
                    o += snprintf(line + o, sizeof(line) - o, "%02x", b[row * 16 + c]);
                line[o] = 0;
                Log("%s\n", line);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[pl] #%ld read fault\n", n);
    }
}

static void WriteBlob(uint8_t* p, int id, void* relay)
{
    int o = 0;
    p[o++] = 0x41; p[o++] = 0x52;
    p[o++] = 0x50;
    p[o++] = 0xB8; memcpy(p + o, &id, 4); o += 4;
    p[o++] = 0x49; p[o++] = 0xBA; o += 8;
    p[o++] = 0x41; p[o++] = 0x52;
    p[o++] = 0x49; p[o++] = 0xBA;
    memcpy(p + o, &relay, 8); o += 8;
    p[o++] = 0x41; p[o++] = 0xFF; p[o++] = 0xE2;
}

static DWORD WINAPI Init(LPVOID)
{
    HANDLE once = CreateMutexA(nullptr, TRUE, "tpf2_payload_probe_single_instance");
    if (once == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_log = _fsopen("C:/Program Files (x86)/Steam/steamapps/workshop/content/1066780/"
                    "3710243057/recon/m4/out/tpf2_payload.log", "w", _SH_DENYWR);
    if (!g_log) return 0;
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[pl] attached, base=%llx\n", (unsigned long long)g_base);

    uint8_t* blob = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blob) { Log("[pl] blob alloc failed\n"); return 0; }
    WriteBlob(blob, 0, (void*)&ApplyRelayProbe);
    void* tramp = nullptr;
    if (InstallHook(g_base + TARGET_RVA, blob, TARGET_STEAL, &tramp)) {
        memcpy(blob + 10, &tramp, 8);
        FlushInstructionCache(GetCurrentProcess(), blob, BLOB_SIZE);
        Log("[pl] hooked payload wrapper rva=%llx steal=%d\n",
            (unsigned long long)TARGET_RVA, TARGET_STEAL);
    } else {
        Log("[pl] HOOK FAILED rva=%llx\n", (unsigned long long)TARGET_RVA);
        return 0;
    }
    for (;;) { Sleep(15000); Log("[pl] alive: hits=%ld detailed=%ld\n", g_hits, g_detailed); }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
