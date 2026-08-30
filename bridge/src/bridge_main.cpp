// M5 bridge DLL: no hooks. Pure file<->UDP bridge.
//   - tails  tpf2_capture_<inst>.txt  (Lua writes local build events here)
//   - sends new lines via reliable UDP to the peer
//   - writes received lines to tpf2_events_<inst>.txt (Lua replays from here)
// Config: tpf2_mp_<dllbasename>.cfg (falls back to tpf2_mp.cfg)
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <memory>
#include <fcntl.h>
#include <io.h>
#include "net.h"
#include "savexfer.h"
#include "simhook.h"
#include "buyhook.h"

static FILE* g_log = nullptr;
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

static FILE* g_events = nullptr;   // peer events out (Lua reads)
static FILE* g_fileOut = nullptr;  // file-relay target (tail writes here when set)
static void OnPeerLine(const char* line)
{
    Log("[net] peer (%zu b): %.200s\n", strlen(line), line);
    if (g_events) {
        // NOTE: stream is binary, so this is a bare LF. The Lua side seeks by
        // byte offset; a CRLF here would desync its offset by one per line.
        fprintf(g_events, "%s\n", line);
        fflush(g_events);
    }
}

struct Config {
    uint16_t localPort = 7771;
    char peerIp[64] = "127.0.0.1";
    uint16_t peerPort = 7772;
    // "auto" = decide from port availability. A better default than "a": the
    // old default silently made every configless bridge claim to be the host.
    std::string instance = "auto";
    std::string tailFile;   // absolute override for tail input (relay mode)
    std::string relayOut;   // absolute path: tail writes here instead of UDP
    // save transfer
    uint16_t    xferPort = 7871;
    std::string saveDir;    // blank = auto-discover Steam userdata save dir
    std::string shareSave;  // blank = newest .sav
    int         autoPull = 0;   // joiner pulls the host's save on startup
    // native sim-thread hook. Off by default: it patches game code, so it must
    // be opted into rather than surprising anyone who just wants replication.
    int         simHook = 0;
    int         buyHook = 0;   // probe the buyVehicle command factory
};

static std::string BaseName(const wchar_t* path)
{
    std::wstring ws(path);
    size_t slash = ws.find_last_of(L'\\');
    std::wstring b = (slash == std::wstring::npos) ? ws : ws.substr(slash + 1);
    size_t dot = b.find_last_of(L'.');
    if (dot != std::wstring::npos) b = b.substr(0, dot);
    return std::string(b.begin(), b.end());
}

static void LoadConfig(const wchar_t* dllPath, Config& cfg)
{
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, dllPath);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    // Sidecar lookup. This used to build only "tpf2_mp_<basename>.cfg", which
    // for a dll named tpf2_bridge_a6.dll means "tpf2_mp_tpf2_bridge_a6.cfg" --
    // a name nothing on disk ever had, so every per-dll config was silently
    // ignored and every bridge fell through to the shared tpf2_mp.cfg (no
    // instance=, no relay settings). Try the real conventions in order.
    std::string base = BaseName(dllPath);
    std::string suffix = base;                       // "a6" from tpf2_bridge_a6
    for (const char* prefix : { "tpf2_bridge_", "tpf2_mp_" }) {
        size_t n = strlen(prefix);
        if (suffix.size() > n && suffix.compare(0, n, prefix) == 0) {
            suffix = suffix.substr(n);
            break;
        }
    }

    const std::string candidates[] = {
        base + ".cfg",              // tpf2_bridge_a6.cfg
        "tpf2_mp_" + suffix + ".cfg",   // tpf2_mp_a6.cfg
        "tpf2_mp_" + base + ".cfg",     // legacy (kept so old names still work)
        "tpf2_mp.cfg",              // shared fallback
    };

    wchar_t path[MAX_PATH];
    FILE* f = nullptr;
    std::string used;
    for (const std::string& c : candidates) {
        swprintf(path, MAX_PATH, L"%s\\%S", dir, c.c_str());
        _wfopen_s(&f, path, L"rb");
        if (f) { used = c; break; }
    }
    if (!f) {
        Log("[cfg] NO CFG FOUND -- using built-in defaults (inst=%s local=%d)\n",
            cfg.instance.c_str(), cfg.localPort);
        return;
    }
    Log("[cfg] loaded %s\n", used.c_str());
    if (used == "tpf2_mp.cfg") {
        Log("[cfg] WARNING: fell back to the shared cfg; per-dll settings "
            "(instance, tail_file, relay_out) are NOT in effect\n");
    }
    char line[256];
    char inst[32];
    char pathBuf[240];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "local_port=%d", &v) == 1) cfg.localPort = (uint16_t)v;
        else if (sscanf(line, "peer_port=%d", &v) == 1) cfg.peerPort = (uint16_t)v;
        else if (sscanf(line, "peer_ip=%63s", cfg.peerIp) == 1) {}
        else if (sscanf(line, "instance=%31s", inst) == 1) cfg.instance = inst;
        else if (sscanf(line, "tail_file=%239s", pathBuf) == 1) cfg.tailFile = pathBuf;
        else if (sscanf(line, "relay_out=%239s", pathBuf) == 1) cfg.relayOut = pathBuf;
        else if (sscanf(line, "xfer_port=%d", &v) == 1) cfg.xferPort = (uint16_t)v;
        else if (sscanf(line, "auto_pull=%d", &v) == 1) cfg.autoPull = v;
        else if (sscanf(line, "sim_hook=%d", &v) == 1) cfg.simHook = v;
        else if (sscanf(line, "buy_hook=%d", &v) == 1) cfg.buyHook = v;
        else if (sscanf(line, "share_save=%239[^\r\n]", pathBuf) == 1) cfg.shareSave = pathBuf;
        else if (sscanf(line, "save_dir=%239[^\r\n]", pathBuf) == 1) cfg.saveDir = pathBuf;
    }
    fclose(f);
}

static FILE* OpenShared(const wchar_t* path, const wchar_t* mode, int osfFlags)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    int fd = _open_osfhandle((intptr_t)h, osfFlags);
    return fd >= 0 ? _fdopen(fd, "r+b") : nullptr;
}

// tail thread: forward new lines from the capture file to the peer
struct TailJob { const wchar_t* path; };
static DWORD WINAPI TailThread(LPVOID arg)
{
    std::wstring capPath = (const wchar_t*)arg;
    delete[] (const wchar_t*)arg;
    uint64_t offset = 0;
    bool started = false;
    std::string line;
    for (;;) {
        Sleep(25);
        FILE* f = nullptr;
        _wfopen_s(&f, capPath.c_str(), L"rb");
        if (!f) continue;
        _fseeki64(f, 0, SEEK_END);
        uint64_t size = (uint64_t)_ftelli64(f);
        if (!started) {
            offset = size;      // skip history on first look
            started = true;
        } else if (size < offset) {
            // file was truncated or replaced (peer bridge restarted). Without
            // this the offset stays past EOF and the tail silently dies.
            Log("[tail] source shrank (%llu < %llu), rewinding to 0\n",
                (unsigned long long)size, (unsigned long long)offset);
            offset = 0;
        }
        _fseeki64(f, offset, SEEK_SET);
        for (;;) {
            int64_t lineStart = _ftelli64(f);
            // read a line of ANY length: capture lines for modular stations
            // run to several KB, and a fixed buffer here used to make the
            // tail re-read the same oversized line forever.
            line.clear();
            bool sawNewline = false;
            for (;;) {
                int c = fgetc(f);
                if (c == EOF) break;
                if (c == '\n') { sawNewline = true; break; }
                line.push_back((char)c);
            }
            // partial line (writer mid-flush)? leave it for next iteration
            if (!sawNewline) { offset = (uint64_t)lineStart; break; }
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) { offset = (uint64_t)_ftelli64(f); continue; }
            if (g_fileOut) {
                fprintf(g_fileOut, "%s\n", line.c_str());
                fflush(g_fileOut);
                Log("[relay] (%zu b) %.200s\n", line.size(), line.c_str());
            } else {
                Net_QueueLine(line.c_str());
                Log("[tail] sent (%zu b): %.200s\n", line.size(), line.c_str());
            }
            offset = (uint64_t)_ftelli64(f);
        }
        fclose(f);
    }
    return 0;
}

static DWORD WINAPI InitThread(LPVOID)
{
    wchar_t dllPath[MAX_PATH] = {0};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&InitThread, &self);
    GetModuleFileNameW(self, dllPath, MAX_PATH);

    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, dllPath);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    // Both instances can share this directory (the alut proxy loads the same
    // bridge into both games), and fopen("a") on Windows is seek-then-write,
    // not an atomic append -- two processes silently overwrite each other's
    // lines. FILE_APPEND_DATA appends atomically, so interleaving is safe.
    wchar_t logPath[MAX_PATH];
    swprintf(logPath, MAX_PATH, L"%s\\tpf2_bridge.log", dir);
    g_log = nullptr;
    {
        HANDLE lh = CreateFileW(logPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lh != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle((intptr_t)lh, _O_WRONLY | _O_APPEND | _O_BINARY);
            if (fd >= 0) g_log = _fdopen(fd, "ab");
        }
    }

    Config cfg;
#ifdef HARDCODE_B
    // sandboxed instance can't read sidecar cfg (Sandboxie file isolation)
    cfg.instance = "b";
    cfg.localPort = 7772;
    cfg.peerPort = 7771;
    strcpy_s(cfg.peerIp, "127.0.0.1");
    Log("[cfg] hardcoded B\n");
#else
    LoadConfig(dllPath, cfg);
#endif
    // Auto identity. With the alut proxy the *same* dll loads into both games,
    // so identity can no longer come from which file was injected where --
    // which is just as well, since getting that wrong was the single most
    // confusing failure mode this project has had. Whoever claims the host
    // port first is "a"; the other is "b".
    if (cfg.instance == "auto") {
        const uint16_t hostPort = cfg.localPort;      // base port from cfg
        const uint16_t guestPort = cfg.peerPort;
        if (Net_PortAvailable(hostPort)) {
            cfg.instance = "a";
            cfg.localPort = hostPort;
            cfg.peerPort = guestPort;
        } else {
            cfg.instance = "b";
            cfg.localPort = guestPort;
            cfg.peerPort = hostPort;
        }
        Log("[m5] auto identity: port %u %s -> instance %s\n", hostPort,
            cfg.instance == "a" ? "free" : "taken", cfg.instance.c_str());
    }

    Log("[m5] bridge init: inst=%s local=%d peer=%s:%d pid=%lu\n",
        cfg.instance.c_str(), cfg.localPort, cfg.peerIp, cfg.peerPort,
        GetCurrentProcessId());
    // The mod's BASE constant must resolve to this same directory, otherwise
    // the two halves talk past each other. Log it so a mismatch is obvious.
    Log("[m5] data dir: %S\n", dir);

    // identity file: the (single) bridge mod reads this to learn which
    // instance it is. Written into the dll's dir — inside a sandbox this
    // lands in the overlay, which is exactly the view the mod shares.
    {
        wchar_t idPath[MAX_PATH];
        swprintf(idPath, MAX_PATH, L"%s\\tpf2_instance.txt", dir);

        // If this file already names a DIFFERENT instance, we are almost
        // certainly injected into the wrong process (the a/b DLLs share a
        // directory, and the mod latches identity from here). Say so loudly:
        // a silent overwrite points the other instance at the wrong pair of
        // capture/event files and looks exactly like "the bridge is dead".
        char prev[64] = {0};
        HANDLE ph = CreateFileW(idPath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ph != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            ReadFile(ph, prev, sizeof(prev) - 1, &got, nullptr);
            CloseHandle(ph);
            char* nl = strpbrk(prev, "\r\n");
            if (nl) *nl = 0;
            if (prev[0] && cfg.instance != prev) {
                Log("[m5] WARNING: identity file said '%s', now claiming '%s' "
                    "-- wrong process? (pid %lu)\n",
                    prev, cfg.instance.c_str(), GetCurrentProcessId());
            }
        }

        HANDLE ih = CreateFileW(idPath, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ih != INVALID_HANDLE_VALUE) {
            // line 1 = instance (all the mod reads); line 2 = owning pid, so a
            // mixed-up injection is diagnosable after the fact
            char buf[128];
            int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s\npid=%lu\n",
                                cfg.instance.c_str(), GetCurrentProcessId());
            DWORD written;
            WriteFile(ih, buf, (DWORD)n, &written, nullptr);
            CloseHandle(ih);
        }
    }

    // events file (peer -> Lua); truncate per session so stale events from
    // previous runs don't replay (they already happened once)
    wchar_t evPath[MAX_PATH];
    swprintf(evPath, MAX_PATH, L"%s\\tpf2_events_%S.txt", dir, cfg.instance.c_str());
    HANDLE eh = CreateFileW(evPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (eh != INVALID_HANDLE_VALUE) {
        // _O_BINARY, not _O_TEXT: the Lua side tracks a byte offset into this
        // file, and CRLF translation made every line one byte shorter on read
        // than on disk, so the offset drifted backwards and replayed garbage.
        int fd = _open_osfhandle((intptr_t)eh, _O_RDWR | _O_BINARY);
        if (fd >= 0) g_events = _fdopen(fd, "r+b");
    }
    Log("[m5] events file: %s\n", g_events ? "open (truncated)" : "FAILED");

    // file-relay mode: tail writes directly to a peer events file (no UDP)
    if (!cfg.relayOut.empty()) {
        wchar_t wpath[MAX_PATH];
        mbstowcs(wpath, cfg.relayOut.c_str(), MAX_PATH);
        HANDLE rh = CreateFileW(wpath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (rh != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle((intptr_t)rh, _O_WRONLY | _O_APPEND | _O_BINARY);
            if (fd >= 0) g_fileOut = _fdopen(fd, "ab");
        }
        Log("[m5] relay_out: %s\n", g_fileOut ? "open" : "FAILED");
    }

    if (!Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerLine)) {
        cfg.localPort += 10;
        if (!Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerLine)) {
            Log("[m5] Net_Init FAILED\n");
        }
    }
    Log("[m5] net up (local %d)\n", cfg.localPort);

    // ---- save transfer -----------------------------------------------------
    // The host serves its world so the joiner can start from the same map.
    // Kept off the event channel on purpose: a save is ~178 MB and that
    // channel is line-oriented reliable UDP with a 32-entry ack window.
    {
        if (cfg.saveDir.empty()) cfg.saveDir = Save_FindSaveDir();
        Log("[m5] save dir: %s\n",
            cfg.saveDir.empty() ? "(not found -- set save_dir= in cfg)" : cfg.saveDir.c_str());

        if (cfg.instance == "a") {
            Save_StartServer(cfg.xferPort, cfg.saveDir, cfg.shareSave, Log);
        } else if (cfg.autoPull) {
            struct PullArgs { std::string ip; uint16_t port; std::string dir; };
            auto* pa = new PullArgs{ cfg.peerIp, cfg.xferPort, cfg.saveDir };
            CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                std::unique_ptr<PullArgs> a((PullArgs*)p);
                // the host may still be starting up; retry for a while
                for (int i = 0; i < 60; ++i) {
                    if (Save_Pull(a->ip.c_str(), a->port, a->dir, Log)) return 0;
                    Sleep(1000);
                }
                Log("[xfer] gave up pulling save from host\n");
                return 0;
            }, pa, 0, nullptr);
        }
    }

    // ---- native sim-thread foothold ---------------------------------------
    // Prototype: prove we get a per-tick callback on the Simulation Thread and
    // that it survives. ECS reads go here next -- doing them from any other
    // thread races the sim (docs/M7_NATIVE_STATE.md).
    if (cfg.simHook) {
        if (SimHook_Install(Log)) {
            // Report from OUR thread, not the sim thread. The handler itself
            // only bumps counters; anything doing I/O per tick would be a
            // repeat of M2's 56 GB logging incident.
            CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                uint64_t last = 0;
                bool everRan = false;
                for (;;) {
                    Sleep(10000);
                    uint64_t now = SimHook_TickCount();
                    if (now == last) {
                        if (everRan) Log("[simhook] idle (no sim ticks in 10s)\n");
                        continue;
                    }
                    everRan = true;
                    uint64_t dNoPeer = 0, dOverflow = 0; size_t pending = 0; bool alive = false;
                    Net_Stats(&dNoPeer, &dOverflow, &pending, &alive);
                    Log("[simhook] ticks=%llu (+%llu in 10s) lastFrameTime=%llu | "
                        "peer=%s pending=%zu dropped=%llu/%llu\n",
                        (unsigned long long)now,
                        (unsigned long long)(now - last),
                        (unsigned long long)SimHook_LastFrameTime(),
                        alive ? "up" : "DOWN", pending,
                        (unsigned long long)dNoPeer, (unsigned long long)dOverflow);
                    last = now;
                }
            }, nullptr, 0, nullptr);
        }
    } else {
        Log("[simhook] disabled (sim_hook=0)\n");
    }

    // buyVehicle factory probe. Purchases are invisible to Lua entirely -- an
    // in-depot vehicle is not a world entity -- so this is the only place the
    // event can be observed.
    if (cfg.buyHook) {
        wchar_t buyPath[MAX_PATH];
        swprintf(buyPath, MAX_PATH, L"%s\\tpf2_buy_%S.txt", dir, cfg.instance.c_str());
        BuyHook_Install(Log, buyPath);
        Log("[buy] purchases -> %S\n", buyPath);
    } else {
        Log("[buy] disabled (buy_hook=0)\n");
    }

    // capture file tail (Lua -> peer)
    std::wstring capPath;
    if (!cfg.tailFile.empty()) {
        capPath = std::wstring(cfg.tailFile.begin(), cfg.tailFile.end());
    } else {
        capPath = std::wstring(dir) + L"\\tpf2_capture_"
            + std::wstring(cfg.instance.begin(), cfg.instance.end()) + L".txt";
    }
    wchar_t* arg = new wchar_t[capPath.size() + 1];
    wcscpy_s(arg, capPath.size() + 1, capPath.c_str());
    CreateThread(nullptr, 0, TailThread, arg, 0, nullptr);
    Log("[m5] tailing capture file\n");
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
