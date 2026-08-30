// ---------------------------------------------------------------------------
// cmdlist_probe — is CommandList::Add the single tap for ALL player actions?
//
// A subagent's static analysis says yes: 0x9d2a00 has 81 direct call sites and
// they are, bar two engine-internal ones, exactly the player-action list --
// road, rail, station, depot, module, demolish, terraform, buy/sell vehicle,
// line create/edit. It also claims api.cmd.sendCommand converges here, so UI and
// script commands would be the same objects in the same queue.
//
// M2_RESULTS.md says the OPPOSITE, from a LIVE test: its apply_command and
// make_command hooks "stayed silent through a full build spree -- empirically
// disproven as the UI tap". One of the two is wrong. Static analysis has already
// produced two confident wrong answers on this exact question tonight (a
// call-graph walk that could not cross signals2, and a frequency count that
// mistook SimBuildingSystem::Update2 for a UI handler), so this measures rather
// than argues.
//
// The likeliest reconciliation is that M2 hooked other members of that family
// and never tried 0x9d2a00 itself -- but that is a hypothesis, and the point of
// this probe is to stop hypothesising.
//
// Observe-only: the original always runs.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include "hook.h"

// Prologue boundaries are 2,3,4,5,7,9,11,13,18,25,34. 18 is both a real
// boundary and a size hook.cpp supports, with no RIP-relative bytes before it.
static const uintptr_t TARGET_RVA = 0x9d2a00;
static const int TARGET_STEAL = 18;
static const int BLOB_SIZE = 48;

extern "C" void ApplyRelayProbe();   // applyrelay_probe.asm (passes the caller too)

static FILE* g_log = nullptr;
static long  g_hits = 0;
static uintptr_t g_base = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[640];
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

// The Lua bridge. Measured: 12,975 of 12,976 calls in one session came from
// here, ~100/second while idle, because game scripts issue commands constantly.
// Player actions arrive from UI call sites instead (a road build came from
// 0x459eb7, inside StreetBuilder::UpdateEngine). Filtering this one caller turns
// a firehose into exactly the events worth reading -- and it is the same
// discriminator lockstep will use to tell a player's command from our own
// replayed one.
static const uintptr_t LUA_BRIDGE_CALLER = 0x1126f1a;

// Recursive, because one hop was not enough. Diffing two builds hundreds of
// metres apart showed 12 of 13 fields byte-identical: the Command carries NO
// geometry, it is a handle. The single field that moved was a pointer at
// (cmd+0x30)->+0x20, so the payload is at least two hops out.
//
// The float filter is deliberately wide now. The first pass required
// |v| > 1.0, which would have hidden a normalised direction or a small
// offset -- pre-judging the scale of a value whose scale is the unknown.
static void DumpMem(const char* label, uint64_t p, size_t n, int depth)
{
    if (!p || !Readable((void*)p, n)) {
        Log("[cmd]   %s %llx <unreadable>\n", label, (unsigned long long)p);
        return;
    }
    const uint8_t* b = (const uint8_t*)p;
    char line[220];
    for (size_t row = 0; row * 16 < n; row++) {
        int o = snprintf(line, sizeof(line), "[cmd]   %s+%02x: ", label, (unsigned)(row * 16));
        for (int c = 0; c < 16; c++)
            o += snprintf(line + o, sizeof(line) - o, "%02x", b[row * 16 + c]);
        o += snprintf(line + o, sizeof(line) - o, "  ");
        for (int c = 0; c < 16; c++) {
            uint8_t ch = b[row * 16 + c];
            line[o++] = (ch >= 32 && ch <= 126) ? (char)ch : '.';
        }
        line[o] = 0;
        Log("%s\n", line);
    }
    for (size_t off = 0; off + 4 <= n; off += 4) {
        float f; memcpy(&f, b + off, 4);
        float a = f < 0 ? -f : f;
        if (f == f && a > 0.001f && a < 1000000.0f)
            Log("[cmd]   %s+%02x FLOAT=%.4f\n", label, (unsigned)off, f);
    }
    if (depth <= 0) return;
    for (size_t off = 0; off + 8 <= n; off += 8) {
        uint64_t q; memcpy(&q, b + off, 8);
        uint64_t clean = q & ~1ULL;
        if (clean < 0x10000 || clean > 0x7FFFFFFFFFFFULL) continue;
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "%s+%02x->", label, (unsigned)off);
        DumpMem(lbl, clean, 0x40, depth - 1);
    }
}

extern "C" void ApplyHandlerProbe(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9,
                                  uint64_t id, uint64_t retAddr)
{
    (void)r8; (void)r9; (void)id;
    long n = InterlockedIncrement(&g_hits);
    uint64_t caller = retAddr - g_base;
    if (caller == LUA_BRIDGE_CALLER) return;   // script traffic, not a player action

    __try {
        // rcx = the CommandList, rdx = the Command (sizeof == 0x38).
        char bytes[3 * 0x38 + 1] = { 0 };
        if (rdx && Readable((void*)rdx, 0x38)) {
            const uint8_t* b = (const uint8_t*)rdx;
            int o = 0;
            for (int i = 0; i < 0x38; i++)
                o += snprintf(bytes + o, sizeof(bytes) - o, "%02x", b[i]);
        } else {
            snprintf(bytes, sizeof(bytes), "<unreadable>");
        }
        Log("\n[cmd] ===== PLAYER COMMAND #%ld t=%llums caller_rva=%llx =====\n"
            "[cmd]   cmd=%llx bytes=%s\n",
            n, (unsigned long long)GetTickCount64(), (unsigned long long)caller,
            (unsigned long long)rdx, bytes);

        // The 0x38 bytes are mostly POINTERS plus a -2 sentinel, so the payload
        // is behind them. Chase each qword that looks like a heap address and
        // dump what it points at; that is where the geometry has to live.
        if (rdx && Readable((void*)rdx, 0x38)) {
            for (int off = 0; off < 0x38; off += 8) {
                uint64_t p;
                memcpy(&p, (const uint8_t*)rdx + off, 8);
                uint64_t clean = p & ~1ULL;      // boost/std tag the low bit
                if (clean < 0x10000 || clean > 0x7FFFFFFFFFFFULL) continue;
                char lbl[24];
                snprintf(lbl, sizeof(lbl), "+%02x->", off);
                // depth 2: the payload is at least two hops out (see DumpMem)
                DumpMem(lbl, clean, 0x40, 2);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[cmd] #%ld read fault\n", n);
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
    // Distinct mutex from the other probes so this can coexist with them --
    // chain-injection stacks DIFFERENT hooks fine; what breaks is two copies of
    // the same hook on the same address.
    HANDLE once = CreateMutexA(nullptr, TRUE, "tpf2_cmdlist_probe_single_instance");
    if (once == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_log = _fsopen("C:/Program Files (x86)/Steam/steamapps/workshop/content/1066780/"
                    "3710243057/recon/m4/out/tpf2_cmdlist.log", "w", _SH_DENYWR);
    if (!g_log) return 0;

    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[cmd] attached, base=%llx\n", (unsigned long long)g_base);

    uint8_t* blob = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blob) { Log("[cmd] blob alloc failed\n"); return 0; }
    WriteBlob(blob, 0, (void*)&ApplyRelayProbe);
    void* tramp = nullptr;
    if (InstallHook(g_base + TARGET_RVA, blob, TARGET_STEAL, &tramp)) {
        memcpy(blob + 10, &tramp, 8);
        FlushInstructionCache(GetCurrentProcess(), blob, BLOB_SIZE);
        Log("[cmd] hooked CommandList::Add rva=%llx steal=%d\n",
            (unsigned long long)TARGET_RVA, TARGET_STEAL);
    } else {
        Log("[cmd] HOOK FAILED rva=%llx\n", (unsigned long long)TARGET_RVA);
        return 0;
    }

    for (;;) { Sleep(15000); Log("[cmd] alive: hits=%ld\n", g_hits); }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
