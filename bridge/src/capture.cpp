#include "capture.h"
#include "net.h"
#include "hook.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>

// ---- targets (M1/M2 recon, corrected against probe8 id mapping) ----
// id 0: make_proposal transform step (0xa16d00) — rcx = 4x4 float transform
//       (probe8 id=4: identity matrix + position; fires per preview frame)
// id 1: make_proposal params step   (0xa18ca0) — rdx = params block
//       (probe8 id=8: small-int type/variant ids; fires per preview frame)
// id 2: applyProposal commit        (0x9e76e0) — rdx = proposal object
struct Target { uintptr_t rva; int steal; };
static const Target TARGETS[] = {
    { 0xa16d00, 15 },
    { 0xa18ca0, 21 },
    { 0x9e76e0, 21 },
};
static const int NUM_TARGETS = 3;
static const int BLOB_SIZE = 48;

extern "C" void ApplyRelay();  // applyrelay.asm (shared with m2 probes)

static void (*g_log)(const char*, ...) = nullptr;
static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log("%s", buf);
}

static uint8_t g_transform[64];
static uint8_t g_params[128];
static bool g_haveTransform = false;
static bool g_haveParams = false;
static void (*g_localCb)(const NetEvent&) = nullptr;

void Capture_SetLocalEventCb(void (*cb)(const NetEvent&)) { g_localCb = cb; }

static void WriteBlob(uint8_t* p, int id, void* relay)
{
    int o = 0;
    p[o++] = 0x41; p[o++] = 0x52;                       // push r10
    p[o++] = 0x50;                                      // push rax
    p[o++] = 0xB8; memcpy(p + o, &id, 4); o += 4;       // mov eax, id
    p[o++] = 0x49; p[o++] = 0xBA; o += 8;               // mov r10, imm64 (patched)
    p[o++] = 0x41; p[o++] = 0x52;                       // push r10
    p[o++] = 0x49; p[o++] = 0xBA;                       // mov r10, relay
    memcpy(p + o, &relay, 8); o += 8;
    p[o++] = 0x41; p[o++] = 0xFF; p[o++] = 0xE2;        // jmp r10
}

extern "C" void Capture_Handler(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9, uint64_t id)
{
    __try {        if (id == 0) {
            memcpy(g_transform, (const void*)rcx, sizeof(g_transform));
            g_haveTransform = true;
        } else if (id == 1) {
            memcpy(g_params, (const void*)rdx, sizeof(g_params));
            g_haveParams = true;
        } else if (id == 2) {
            NetEvent ev{};
            ev.type = 1;
            ev.tickMs = GetTickCount64();
            ev.proposalPtr = rdx;
            if (g_haveTransform) memcpy(ev.transform, g_transform, 64);
            if (g_haveParams) memcpy(ev.params, g_params, 128);
            Net_QueueEvent(ev);
            if (g_localCb) g_localCb(ev);
            // one-shot hex dump of the committed proposal object (2KB) for
            // offline analysis — much easier to find fileName/transf in Python
            static uint64_t lastDumped = 0;
            if (rdx != lastDumped) {
                lastDumped = rdx;
                __try {
                    const uint8_t* b = (const uint8_t*)rdx;
                    char line[128];
                    for (int row = 0; row < 128; row++) {
                        int o = 0;
                        o += snprintf(line + o, sizeof(line) - o, "[dump] %04x: ", row * 16);
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
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("[dump] read fault\n");
                }
            }
            const float* f = (const float*)g_transform;
            const int* ip = (const int*)g_params;
            Log("[capture] COMMIT proposal=%llx pos=(%.1f %.1f %.1f) params=(%d %d %d %d)\n",
                (unsigned long long)rdx,
                g_haveTransform ? f[12] : 0.f, g_haveTransform ? f[13] : 0.f,
                g_haveTransform ? f[14] : 0.f,
                g_haveParams ? ip[0] : 0, g_haveParams ? ip[1] : 0,
                g_haveParams ? ip[2] : 0, g_haveParams ? ip[3] : 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[capture] read fault id=%llu\n", (unsigned long long)id);
    }
}

bool Capture_Init(uintptr_t gameBase, void (*logFn)(const char* fmt, ...))
{
    g_log = logFn;
    uint8_t* blobs = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blobs) { Log("[capture] blob alloc failed\n"); return false; }
    int ok = 0;
    for (int i = 0; i < NUM_TARGETS; i++) {
        uint8_t* blob = blobs + i * BLOB_SIZE;
        WriteBlob(blob, i, (void*)&ApplyRelay);
        void* tramp = nullptr;
        if (InstallHook(gameBase + TARGETS[i].rva, blob, TARGETS[i].steal, &tramp)) {
            memcpy(blob + 10, &tramp, 8);
            FlushInstructionCache(GetCurrentProcess(), blob, BLOB_SIZE);
            ok++;
        } else {
            Log("[capture] hook %d FAILED rva=%llx\n", i,
                (unsigned long long)TARGETS[i].rva);
        }
    }
    Log("[capture] %d/%d hooks installed\n", ok, NUM_TARGETS);
    return ok == NUM_TARGETS;
}

// applyrelay.asm calls ApplyHandler — alias it to our capture handler.
extern "C" void ApplyHandler(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9, uint64_t id)
{
    Capture_Handler(rcx, rdx, r8, r9, id);
}
