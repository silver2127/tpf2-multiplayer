// ---------------------------------------------------------------------------
// probe_apply — does the applyProposal choke point still hold on build 35924,
// and what does a MODULAR STATION proposal actually look like?
//
// WHY: the current replication captures STATE (poll the world, diff a cached
// snapshot, guess what the player did) and every hard bug this project has hit
// is the same one -- intent was thrown away at capture and is being
// reconstructed by heuristic (12 shadow tables, 10 TTL constants). REPORT.md §5
// already specified the right design: tap the point where intent EXISTS.
// M2 found it -- applyProposal, RVA 0x9e76e0, 102 paired hits per depot build.
//
// This probe answers the three questions that decide whether the pivot is real,
// before anyone spends a week on it:
//   1. Does 0x9e76e0 still fire on this build?
//   2. Can we identify WHAT was built from the proposal (the .con path)?
//   3. Do SCRIPT-issued builds (api.cmd/game.interface, i.e. our own replay)
//      reach the same hook as UI builds? If yes one tap covers both, and the
//      echo problem collapses to a single flag. If no, capture and replay stay
//      separate paths and the pivot is worth much less.
//
// Deliberately standalone: it links none of the live bridge's networking, so it
// cannot inject anything into replication. Read-only observation.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include "hook.h"

// id 0/1 are the make_proposal steps that carry the transform and the params
// block; they fire once per PREVIEW FRAME while the player drags a building
// around, so they only ever record -- never log. M2 learned that the expensive
// way: an always-on validation hook wrote ~56 GB of renderer noise in a session.
// id 2 is the commit, and it is the only one that says anything.
struct Target { uintptr_t rva; int steal; const char* name; };
static const Target TARGETS[] = {
    { 0xa16d00, 15, "make_proposal.transform" },
    { 0xa18ca0, 21, "make_proposal.params"    },
    { 0x9e76e0, 21, "applyProposal.COMMIT"    },
};
static const int NUM_TARGETS = 3;
static const int BLOB_SIZE = 48;

extern "C" void ApplyRelay();   // applyrelay.asm

static FILE* g_log = nullptr;
static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, g_log);
    fflush(g_log);
}

static uint8_t g_transform[64];
static uint8_t g_params[128];
static volatile long g_hits[NUM_TARGETS] = { 0, 0, 0 };
static int g_commits = 0;

static bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// The money shot. A ".con" path is far longer than the 15-char SSO limit, so
// std::string keeps it on the heap and the proposal holds a POINTER to it --
// which means a plain hex dump shows an address, not the filename. Chase every
// aligned qword that looks like a pointer and see if a string lives there.
// Without this the dump cannot tell a depot from a modular station.
// Chases pointers to DEPTH levels looking for resource paths.
//
// One level found nothing, and the dump says why: the proposal is a graph of
// nested std::vectors, so +0x00 is a pointer to a pointer to a struct that
// eventually holds the std::string. A ".con" path is past the 15-char SSO
// limit, so even the final string is behind one more indirection. Depth 1 was
// never going to reach it.
static void ChaseStrings(const uint8_t* base, size_t len, int depth, const char* trail)
{
    if (depth <= 0) return;
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint64_t p;
        memcpy(&p, base + off, 8);
        if (p < 0x10000 || p > 0x7FFFFFFFFFFFULL) continue;
        if (p & 7) continue;                       // pointers here are aligned
        const char* s = (const char*)p;
        if (!Readable(s, 64)) continue;
        char tmp[97];
        int n = 0;
        while (n < 96 && s[n] >= 32 && s[n] <= 126) { tmp[n] = s[n]; n++; }
        tmp[n] = 0;
        char next[128];
        snprintf(next, sizeof(next), "%s+0x%x", trail, (unsigned)off);
        if (n >= 6 && (strchr(tmp, '/') || strchr(tmp, '.'))) {
            Log("[probe]   PATH %s -> \"%s\"\n", next, tmp);
        } else {
            // not a string here; treat it as another node and go deeper. Only
            // the first 128 bytes of each child, or this explodes.
            ChaseStrings((const uint8_t*)p, 128, depth - 1, next);
        }
    }
}

static void ScanForStrings(const uint8_t* base, size_t len)
{
    ChaseStrings(base, len, 3, "");
}

static void DumpHex(const uint8_t* b, size_t rows)
{
    char line[160];
    for (size_t row = 0; row < rows; row++) {
        if (!Readable(b + row * 16, 16)) break;
        int o = snprintf(line, sizeof(line), "[probe]   %04x: ", (unsigned)(row * 16));
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
}

extern "C" void ApplyHandler(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9, uint64_t id)
{
    if (id < NUM_TARGETS) InterlockedIncrement(&g_hits[id]);
    __try {
        if (id == 0) {
            if (Readable((void*)rcx, sizeof(g_transform)))
                memcpy(g_transform, (const void*)rcx, sizeof(g_transform));
        } else if (id == 1) {
            if (Readable((void*)rdx, sizeof(g_params)))
                memcpy(g_params, (const void*)rdx, sizeof(g_params));
        } else if (id == 2) {
            g_commits++;
            const float* f = (const float*)g_transform;
            const int* ip = (const int*)g_params;
            Log("\n[probe] ===== COMMIT #%d  t=%llums  proposal=%llx =====\n",
                g_commits, (unsigned long long)GetTickCount64(),
                (unsigned long long)rdx);
            Log("[probe]   pos=(%.2f %.2f %.2f)  params[0..7]=%d %d %d %d %d %d %d %d\n",
                f[12], f[13], f[14],
                ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
            Log("[probe]   hits: transform=%ld params=%ld commit=%ld\n",
                g_hits[0], g_hits[1], g_hits[2]);
            if (rdx && Readable((void*)rdx, 512)) {
                // 1 KB of the proposal, then chase its pointers for resource
                // paths. Bounded so a build spree cannot fill the disk.
                if (g_commits <= 40) {
                    DumpHex((const uint8_t*)rdx, 64);
                    ScanForStrings((const uint8_t*)rdx, 1024);
                }
            } else {
                Log("[probe]   proposal ptr not readable\n");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[probe] read fault id=%llu\n", (unsigned long long)id);
    }
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
    char path[MAX_PATH];
    snprintf(path, sizeof(path),
        "C:/Program Files (x86)/Steam/steamapps/workshop/content/1066780/"
        "3710243057/recon/m4/out/tpf2_probe_apply.log");
    // _fsopen with _SH_DENYWR, NOT fopen_s. MSVC's fopen_s opens with
    // EXCLUSIVE sharing, so the game holds the log and nothing else can read it
    // -- the log becomes unreadable until the process exits, which defeats the
    // point of a probe you want to watch live. Every other log in this project
    // is tailed while the game runs; this one must be too.
    g_log = _fsopen(path, "w", _SH_DENYWR);
    if (!g_log) return 0;

    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[probe] attached, game base=%llx\n", (unsigned long long)base);

    uint8_t* blobs = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blobs) { Log("[probe] blob alloc failed\n"); return 0; }

    int ok = 0;
    for (int i = 0; i < NUM_TARGETS; i++) {
        uint8_t* blob = blobs + i * BLOB_SIZE;
        WriteBlob(blob, i, (void*)&ApplyRelay);
        void* tramp = nullptr;
        if (InstallHook(base + TARGETS[i].rva, blob, TARGETS[i].steal, &tramp)) {
            memcpy(blob + 10, &tramp, 8);
            FlushInstructionCache(GetCurrentProcess(), blob, BLOB_SIZE);
            Log("[probe] hooked %-24s rva=%llx\n", TARGETS[i].name,
                (unsigned long long)TARGETS[i].rva);
            ok++;
        } else {
            Log("[probe] hook FAILED %-24s rva=%llx  <- prologue changed on this build?\n",
                TARGETS[i].name, (unsigned long long)TARGETS[i].rva);
        }
    }
    Log("[probe] %d/%d hooks installed\n", ok, NUM_TARGETS);

    // Heartbeat, so "no commits" is distinguishable from "probe never ran" --
    // the exact ambiguity that wasted a cycle on the Lua side today.
    for (;;) {
        Sleep(10000);
        Log("[probe] alive: transform=%ld params=%ld commit=%ld\n",
            g_hits[0], g_hits[1], g_hits[2]);
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
