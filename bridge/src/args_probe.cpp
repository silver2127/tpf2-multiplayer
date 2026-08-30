// ---------------------------------------------------------------------------
// args_probe — dump what the caller HANDS to make_cmd::BuildProposal.
//
// Every previous attempt chased the factory's OUTPUT: the 56-byte Command (a
// handle, 12 of 13 fields identical across two builds), then the 0xB18 payload
// (pointer-dense; 61 differing rows, all heap addresses, no coordinates). That
// was dumping the packaging and looking for the contents.
//
// Reading the decompiled factory instead shows the data arrives as arguments:
//     FUN_1403e7e10(local_b68, a2);          a2 -> the 0x2F8 proposal struct
//     FUN_14045f3a0(local_870, a3);          a3 -> a second structure
//     plVar4 = *(longlong **)(a3 + 0x68);    a3 dereferenced at +0x68
//     FUN_1403e3d30(a3 + 0x18);              and at +0x18
//
// +0x18 and +0x68 match the offsets an earlier dump attributed to `in_R9`, so a3
// IS r9 and is a caller-owned structure built from player input -- not something
// the factory manufactures.
//
// ABI: the return is a 0x38 struct, so rcx is the hidden return pointer and the
// real arguments shift: rdx = a1, r8 = a2, r9 = a3.
//
// Observe-only. Two builds in different places, then diff: whatever moves is the
// geometry, which holds regardless of whether the layout guess is right.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include "hook.h"

// EVERY command factory, not just BuildProposal.
//
// Per ACTION_MAP.md the whole construction family -- road, rail, station, depot,
// station module, bus stop, signals, demolish, terraform -- produces a
// BuildProposal command, so 0x9dc750 alone covers all of it. Vehicles and lines
// use separate factories, listed here so one session captures everything.
//
// Steal sizes were checked individually against each prologue; all are real
// instruction boundaries that hook.cpp supports, with no RIP-relative bytes
// inside the stolen range.
//
// Same ABI throughout: these return a 0x38 Command by value, so rcx is the
// hidden return pointer and the real arguments are rdx=a1, r8=a2, r9=a3.
struct Target { uintptr_t rva; int steal; const char* name; };
static const Target TARGETS[] = {
    { 0x9dc750, 19, "BuildProposal" },   // all construction + demolish + terraform
    { 0x9dca00, 15, "BuyVehicle"    },
    { 0x9de380, 20, "SellVehicle"   },
    { 0x9dddb0, 15, "ReplaceVehicle"},
    { 0x9dea10, 18, "SetLine"       },
    { 0x9dcde0, 19, "CreateLine"    },
    { 0x9df4e0, 19, "UpdateLine"    },
    { 0x9dd190, 20, "DeleteLine"    },
    { 0x9de6f0, 20, "SendToDepot"   },
};
static const int NUM_TARGETS = 9;
static const int BLOB_SIZE = 48;
static const int MAX_DUMPS = 40;   // many action types in one session

extern "C" void ApplyRelayProbe();

static FILE* g_log = nullptr;
static long g_hits = 0, g_dumps = 0;
static uintptr_t g_base = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[700];
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

static void Dump(const char* label, uint64_t p, size_t n)
{
    if (!p) { Log("[arg] %s = NULL\n", label); return; }
    if (!Readable((void*)p, n)) { Log("[arg] %s = %llx <unreadable>\n", label,
                                      (unsigned long long)p); return; }
    Log("[arg] --- %s = %llx ---\n", label, (unsigned long long)p);
    const uint8_t* b = (const uint8_t*)p;
    char line[160];
    for (size_t row = 0; row * 16 < n; row++) {
        int o = snprintf(line, sizeof(line), "[arg] %s %03x: ", label, (unsigned)(row * 16));
        for (int c = 0; c < 16; c++)
            o += snprintf(line + o, sizeof(line) - o, "%02x", b[row * 16 + c]);
        line[o] = 0;
        Log("%s\n", line);
    }
}

extern "C" void ApplyHandlerProbe(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9,
                                  uint64_t id, uint64_t retAddr)
{
    long n = InterlockedIncrement(&g_hits);
    if (g_dumps >= MAX_DUMPS) return;
    const char* what = (id < NUM_TARGETS) ? TARGETS[id].name : "?";
    __try {
        g_dumps++;
        // The command NAME is what makes a multi-action session readable: every
        // construction type produces BuildProposal, so the caller_rva is what
        // separates a station from a demolish, and the name separates
        // construction from a vehicle or line command.
        Log("\n[arg] ===== #%ld %s t=%llums caller_rva=%llx ret=%llx a1=%llx a2=%llx a3=%llx =====\n",
            n, what, (unsigned long long)GetTickCount64(),
            (unsigned long long)(retAddr - g_base),
            (unsigned long long)rcx, (unsigned long long)rdx,
            (unsigned long long)r8, (unsigned long long)r9);

        // a2 == a3 + 0x70 in every construction capture so far, so these are not
        // two arguments but one proposal object viewed at two offsets. The street
        // half (nodes, edges) lives in the first 0x100; the construction half --
        // the .con list -- must be past it, which is why 0x100 never showed it.
        Dump("a2", r8, 0x80);
        Dump("a3", r9, 0x800);

        // Chase pointers looking for resource paths.
        //
        // A station's identity is a .con path, and no ASCII appeared anywhere in
        // the first 0x100 bytes of a2/a3 -- because a path exceeds the 15-char
        // SSO limit, so std::string keeps it on the heap and only the POINTER is
        // inline. Roads did not need this (their payload is a node vector), but
        // for a station the question is "which .con, where, with what params",
        // which is a different shape.
        //
        // Two levels: a std::string member inside a struct that is itself
        // referenced from a2/a3 is one hop further than a direct field.
        for (int which = 0; which < 2; which++) {
            uint64_t basep = which ? r9 : r8;
            const char* nm = which ? "a3" : "a2";
            for (unsigned off = 0; off + 8 <= 0x800; off += 8) {
                if (!Readable((void*)(basep + off), 8)) continue;
                uint64_t p1 = 0; memcpy(&p1, (void*)(basep + off), 8);
                uint64_t c1 = p1 & ~1ULL;
                if (c1 < 0x10000 || c1 > 0x7FFFFFFFFFFFULL) continue;
                // level 1: is a readable string sitting right here?
                if (Readable((void*)c1, 64)) {
                    const char* s = (const char*)c1;
                    int k = 0; while (k < 63 && s[k] >= 32 && s[k] <= 126) k++;
                    if (k >= 6 && (memchr(s, '/', k) || memchr(s, '.', k))) {
                        char tmp[72]; memcpy(tmp, s, k); tmp[k] = 0;
                        Log("[arg]   PATH %s+%03x -> \"%s\"\n", nm, off, tmp);
                        continue;
                    }
                }
                // level 2: treat it as a struct and look one hop deeper
                for (unsigned o2 = 0; o2 + 8 <= 0x60; o2 += 8) {
                    if (!Readable((void*)(c1 + o2), 8)) continue;
                    uint64_t p2 = 0; memcpy(&p2, (void*)(c1 + o2), 8);
                    uint64_t c2 = p2 & ~1ULL;
                    if (c2 < 0x10000 || c2 > 0x7FFFFFFFFFFFULL) continue;
                    if (!Readable((void*)c2, 64)) continue;
                    const char* s2 = (const char*)c2;
                    int k2 = 0; while (k2 < 63 && s2[k2] >= 32 && s2[k2] <= 126) k2++;
                    if (k2 >= 6 && (memchr(s2, '/', k2) || memchr(s2, '.', k2))) {
                        char tmp[72]; memcpy(tmp, s2, k2); tmp[k2] = 0;
                        Log("[arg]   PATH %s+%03x+%03x -> \"%s\"\n", nm, off, o2, tmp);
                    }
                }
            }
        }

        // Find std::vectors by SHAPE and dump their elements.
        //
        // Diffing two builds showed the differing qwords in a2/a3 are not noise:
        // consecutive pairs are vector begin/end. a2+0x00 gave begin/end 0x48
        // apart in BOTH builds at different addresses -- a 72-byte container
        // allocated fresh per build, which is what per-build geometry looks like.
        // a3+0x30 showed the same with a 0x80 span.
        //
        // Detect rather than hardcode: treat every aligned pair as a candidate
        // begin/end and accept it only if the span is positive, modest, and a
        // multiple of 4. Guessed offsets have been wrong twice tonight; a shape
        // test that the data itself has to satisfy is harder to fool.
        for (int which = 0; which < 2; which++) {
            uint64_t basep = which ? r9 : r8;
            const char* nm = which ? "a3" : "a2";
            for (unsigned off = 0; off + 16 <= 0x800; off += 8) {
                if (!Readable((void*)(basep + off), 16)) continue;
                uint64_t b0 = 0, e0 = 0;
                memcpy(&b0, (void*)(basep + off), 8);
                memcpy(&e0, (void*)(basep + off + 8), 8);
                if (b0 < 0x10000 || e0 <= b0) continue;
                uint64_t span = e0 - b0;
                // The old cap here was 0x800, and it silently dropped the one
                // vector that mattered: the station's edge list is 2880 bytes
                // (24 edges x 120), so it was rejected while the 120-byte depot
                // equivalent came through. Fourth time this session that a limit
                // chosen for tidiness has hidden the answer -- so keep the shape
                // test, but make the ceiling far larger than any real payload and
                // truncate the OUTPUT instead of the detection.
                if (span > 0x20000 || (span & 3)) continue;
                if (!Readable((void*)b0, (size_t)span)) continue;
                Log("[arg] VEC %s+%03x begin=%llx end=%llx span=%llu\n",
                    nm, off, (unsigned long long)b0, (unsigned long long)e0,
                    (unsigned long long)span);
                const uint8_t* eb = (const uint8_t*)b0;
                size_t shown = span > 0x400 ? 0x400 : (size_t)span;
                char line[160];
                for (size_t row = 0; row * 16 < shown; row++) {
                    int o = snprintf(line, sizeof(line), "[arg]   %s+%03x %03x: ",
                                     nm, off, (unsigned)(row * 16));
                    for (int c = 0; c < 16 && row * 16 + c < shown; c++)
                        o += snprintf(line + o, sizeof(line) - o, "%02x", eb[row * 16 + c]);
                    line[o] = 0;
                    Log("%s\n", line);
                }
                if (shown < span)
                    Log("[arg]   %s+%03x ... %llu more bytes\n", nm, off,
                        (unsigned long long)(span - shown));
                for (size_t k = 0; k + 4 <= shown; k += 4) {
                    float f; memcpy(&f, eb + k, 4);
                    float av = f < 0 ? -f : f;
                    if (f == f && av > 0.5f && av < 200000.0f)
                        Log("[arg]   %s+%03x F%03x=%.3f\n", nm, off, (unsigned)k, f);
                }
                // A construction entry carries its .con as a std::string, so the
                // path is a pointer INSIDE an element -- not inside a2/a3, which
                // is the only place the earlier chase looked. That is why two
                // levels from the arguments found street materials and no .con.
                for (size_t k = 0; k + 8 <= span && k < 0x2000; k += 8) {
                    uint64_t p = 0; memcpy(&p, eb + k, 8);
                    if (p < 0x10000 || p > 0x7FFFFFFFFFFFULL) continue;
                    if (!Readable((void*)p, 64)) continue;
                    const char* s = (const char*)p;
                    int j = 0; while (j < 63 && s[j] >= 32 && s[j] <= 126) j++;
                    // No '.' requirement. Demanding one found the .con path but
                    // filtered out every module name, which is the half of the
                    // params that actually varies per build -- the fifth time a
                    // convenience filter has hidden the answer tonight.
                    if (j >= 4) {
                        char tmp[72]; memcpy(tmp, s, j); tmp[j] = 0;
                        Log("[arg]   STR %s+%03x[%03x] -> \"%s\"\n",
                            nm, off, (unsigned)k, tmp);
                    }
                }
                // A vector<std::string> has stride 32 on MSVC: a 16-byte union
                // that is either the text itself or a pointer to it, then size,
                // then capacity. Short module names live INLINE, so chasing
                // pointers can never see them -- they must be read as strings.
                if ((span % 32) == 0 && span >= 32) {
                    for (size_t e = 0; e + 32 <= span && e < 0x1000; e += 32) {
                        uint64_t sz = 0, cap = 0;
                        memcpy(&sz,  eb + e + 0x10, 8);
                        memcpy(&cap, eb + e + 0x18, 8);
                        if (cap < 15 || cap > 0x1000 || sz > cap) continue;
                        const char* s = nullptr;
                        if (cap == 15) {
                            s = (const char*)(eb + e);          // inline
                        } else {
                            uint64_t hp = 0; memcpy(&hp, eb + e, 8);
                            if (hp < 0x10000 || !Readable((void*)hp, sz + 1)) continue;
                            s = (const char*)hp;
                        }
                        char tmp[80];
                        size_t take = sz < sizeof(tmp) - 1 ? sz : sizeof(tmp) - 1;
                        memcpy(tmp, s, take); tmp[take] = 0;
                        bool ok = true;
                        for (size_t q = 0; q < take; q++)
                            if (tmp[q] < 32 || tmp[q] > 126) { ok = false; break; }
                        if (ok && take > 0)
                            Log("[arg]   SSTR %s+%03x[%03x] len=%llu \"%s\"\n",
                                nm, off, (unsigned)e, (unsigned long long)sz, tmp);
                    }
                }
            }
        }

        // The two fields the factory actually dereferences on a3. Following them
        // is not a guess -- the decompiled code names these offsets.
        if (Readable((void*)(r9 + 0x18), 8)) {
            uint64_t p18 = 0; memcpy(&p18, (void*)(r9 + 0x18), 8);
            Log("[arg] a3+0x18 = %llx\n", (unsigned long long)p18);
            if (p18 > 0x10000) Dump("a3_18", p18, 0x80);
        }
        if (Readable((void*)(r9 + 0x68), 8)) {
            uint64_t p68 = 0; memcpy(&p68, (void*)(r9 + 0x68), 8);
            Log("[arg] a3+0x68 = %llx\n", (unsigned long long)p68);
            if (p68 > 0x10000) Dump("a3_68", p68, 0x80);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[arg] #%ld read fault\n", n);
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
    HANDLE once = CreateMutexA(nullptr, TRUE, "tpf2_args_probe_single_instance");
    if (once == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_log = _fsopen("C:/Program Files (x86)/Steam/steamapps/workshop/content/1066780/"
                    "3710243057/recon/m4/out/tpf2_args.log", "w", _SH_DENYWR);
    if (!g_log) return 0;
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[arg] attached, base=%llx\n", (unsigned long long)g_base);

    uint8_t* blobs = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blobs) { Log("[arg] blob alloc failed\n"); return 0; }
    int ok = 0;
    for (int i = 0; i < NUM_TARGETS; i++) {
        uint8_t* blob = blobs + i * BLOB_SIZE;
        WriteBlob(blob, i, (void*)&ApplyRelayProbe);
        void* tramp = nullptr;
        if (InstallHook(g_base + TARGETS[i].rva, blob, TARGETS[i].steal, &tramp)) {
            memcpy(blob + 10, &tramp, 8);
            FlushInstructionCache(GetCurrentProcess(), blob, BLOB_SIZE);
            Log("[arg] hooked %-15s rva=%llx steal=%d\n", TARGETS[i].name,
                (unsigned long long)TARGETS[i].rva, TARGETS[i].steal);
            ok++;
        } else {
            // Report rather than abort: one bad prologue must not cost the other
            // eight, and knowing WHICH failed is the useful part.
            Log("[arg] HOOK FAILED %-15s rva=%llx steal=%d\n", TARGETS[i].name,
                (unsigned long long)TARGETS[i].rva, TARGETS[i].steal);
        }
    }
    Log("[arg] %d/%d hooks installed\n", ok, NUM_TARGETS);
    if (ok == 0) return 0;
    for (;;) { Sleep(15000); Log("[arg] alive: hits=%ld dumps=%ld\n", g_hits, g_dumps); }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
