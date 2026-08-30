// M4a replication DLL entry: config, capture hooks, net thread, logging.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <fcntl.h>
#include <io.h>
#include "capture.h"
#include "net.h"

static FILE* g_log = nullptr;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[768];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, g_log);
    fflush(g_log);
}

// called from net thread for every in-order event from the peer
static FILE* g_events = nullptr;

static void WriteEventLine(const char* origin, const NetEvent& ev)
{
    if (!g_events) return;
    const float* f = (const float*)ev.transform;
    const int* ip = (const int*)ev.params;
    // one line, space-separated; the Lua replay mod parses this
    fprintf(g_events, "BUILD %s tick=%llu pos=%.3f %.3f %.3f rot=%.3f %.3f %.3f %.3f params=%d %d %d %d %d %d %d %d\n",
        origin, (unsigned long long)ev.tickMs,
        f[12], f[13], f[14], f[0], f[5], f[10], f[15],
        ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
    fflush(g_events);
}

static void OnPeerEvent(const NetEvent& ev)
{
    const float* f = (const float*)ev.transform;
    const int* ip = (const int*)ev.params;
    Log("[net] PEER BUILD tick=%llu proposal=%llx pos=(%.1f %.1f %.1f) params=(%d %d %d %d)\n",
        (unsigned long long)ev.tickMs, (unsigned long long)ev.proposalPtr,
        f[12], f[13], f[14], ip[0], ip[1], ip[2], ip[3]);
    WriteEventLine("peer", ev);
}

struct Config {
    uint16_t localPort = 7771;
    char peerIp[64] = "127.0.0.1";
    uint16_t peerPort = 7772;
    bool loopback = false;   // also write LOCAL commits to the event file
};

static bool LoadConfig(const wchar_t* dllDir, Config& cfg)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\tpf2_mp.cfg", dllDir);
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rb");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "local_port=%d", &v) == 1) cfg.localPort = (uint16_t)v;
        else if (sscanf(line, "peer_port=%d", &v) == 1) cfg.peerPort = (uint16_t)v;
        else if (sscanf(line, "peer_ip=%63s", cfg.peerIp) == 1) {}
        else if (sscanf(line, "loopback=%d", &v) == 1) cfg.loopback = (v != 0);
    }
    fclose(f);
    return true;
}

static DWORD WINAPI InitThread(LPVOID)
{
    wchar_t dllDir[MAX_PATH] = {0};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&InitThread, &self);
    GetModuleFileNameW(self, dllDir, MAX_PATH);
    wchar_t* slash = wcsrchr(dllDir, L'\\');
    if (slash) *slash = 0;

    wchar_t logPath[MAX_PATH];
    swprintf(logPath, MAX_PATH, L"%s\\tpf2_mp.log", dllDir);
    HANDLE h = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_TEXT);
        if (fd >= 0) g_log = _fdopen(fd, "wb");
    }

    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[m4a] init: base=%llx\n", (unsigned long long)base);

    Config cfg;
    bool hasCfg = LoadConfig(dllDir, cfg);
    Log("[m4a] config %s: local=%d peer=%s:%d loopback=%d\n",
        hasCfg ? "loaded" : "MISSING (using defaults)", cfg.localPort,
        cfg.peerIp, cfg.peerPort, cfg.loopback ? 1 : 0);

    // event file for the Lua replay bridge (share-read so Lua can poll it)
    wchar_t evPath[MAX_PATH];
    swprintf(evPath, MAX_PATH, L"%s\\tpf2_events.txt", dllDir);
    HANDLE eh = CreateFileW(evPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (eh != INVALID_HANDLE_VALUE) {
        int fd = _open_osfhandle((intptr_t)eh, _O_WRONLY | _O_APPEND | _O_TEXT);
        if (fd >= 0) g_events = _fdopen(fd, "ab");
    }
    Log("[m4a] event file: %s\n", g_events ? "open" : "FAILED");

    if (!Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerEvent)) {
        // port may be held by a stale instance of ourselves (dev iterations
        // in the same game process) — fall back to +10
        cfg.localPort += 10;
        Log("[m4a] Net_Init failed on cfg port, retrying %d\n", cfg.localPort);
        if (!Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerEvent)) {
            Log("[m4a] Net_Init FAILED\n");
            return 1;
        }
    }
    Log("[m4a] net up (local %d)\n", cfg.localPort);

    if (!Capture_Init(base, Log)) {
        Log("[m4a] Capture_Init FAILED (partial)\n");
    }
    if (cfg.loopback) Capture_SetLocalEventCb([](const NetEvent& ev) {
        WriteEventLine("local", ev);
    });
    Log("[m4a] ready\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        Net_Shutdown();
    }
    return TRUE;
}
