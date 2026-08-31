// M5 bridge DLL: no hooks. Pure file<->UDP bridge.
//   - tails  tpf2_capture_<inst>.txt  (Lua writes local build events here)
//   - sends new lines via reliable UDP to the peer
//   - writes received lines to tpf2_events_<inst>.txt (Lua replays from here)
// Config: tpf2_mp_<dllbasename>.cfg (falls back to tpf2_mp.cfg), looked up in
// the DLL's own directory (CFGDIR, where the installer puts it) and then in
// the data dir (user override).
//
// Two directories, deliberately:
//   CFGDIR  = the directory this DLL lives in (game dir; may be read-only)
//   DATADIR = Tpf2mpDataDirW (%LOCALAPPDATA%\tpf2mp\data, or TPF2MP_DATADIR)
// Every file written at run time -- log, identity, events, captures, buy
// probe, control file -- lives in DATADIR. Nothing is ever written to CFGDIR.
//
// Runtime control: DATADIR\tpf2_bridge_ctl.txt, polled every 500 ms, lines
//   instance=a|b        re-identify (rewrite identity, new events file, retarget tail)
//   peer=<ipv4>:<port>  repoint the UDP peer without restarting the socket
// The cfg keys instance=/peer_ip=/peer_port= stay the initial values.
// Identity: DATADIR\tpf2_instance.txt, lines '<a|b>', 'pid=<pid>',
// 'port=<bound UDP port>' (line 3 is what the lobby routes peer frames to).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>
#include <fcntl.h>
#include <io.h>
#include "net.h"
#include "datadir.h"
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

// Mask the host part of a public IPv4 for the log. tpf2_bridge.log gets pasted
// into bug reports and screenshots, and a player's home address has no business
// travelling with it -- "203.0.113.x" still says which peer a line is about.
// Loopback and the RFC1918 ranges are left readable: they identify nobody and
// masking them would only make local debugging harder. TPF2MP_LOG_IPS=1 keeps
// the full address for a NAT problem that genuinely needs it.
static const char* RedactIp(const char* ip)
{
    static char buf[4][64];
    static int slot = 0;
    if (!ip || !*ip) return "";
    static int show = -1;
    if (show < 0) {
        char v[8] = {0}; DWORD n = GetEnvironmentVariableA("TPF2MP_LOG_IPS", v, sizeof(v));
        show = (n > 0 && v[0] == '1') ? 1 : 0;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (show || sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return ip;
    const bool priv = (a == 127) || (a == 10) || (a == 192 && b == 168)
                   || (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31);
    if (priv) return ip;
    slot = (slot + 1) % 4;
    _snprintf_s(buf[slot], sizeof(buf[slot]), _TRUNCATE, "%u.%u.%u.x", a, b, c);
    return buf[slot];
}

static FILE* g_events = nullptr;   // peer events out (Lua reads)
static std::mutex g_eventsMtx;     // OnPeerLine (net thread) vs re-identify (ctl thread)
static FILE* g_fileOut = nullptr;  // file-relay target (tail writes here when set)
static void OnPeerLine(const char* line)
{
    Log("[net] peer (%zu b): %.200s\n", strlen(line), line);
    std::lock_guard<std::mutex> lk(g_eventsMtx);
    if (g_events) {
        // NOTE: stream is binary, so this is a bare LF. The Lua side seeks by
        // byte offset; a CRLF here would desync its offset by one per line.
        fprintf(g_events, "%s\n", line);
        fflush(g_events);
    }
}

// DATADIR with trailing backslash (datadir.h contract). Set once in InitThread
// before any thread that formats paths is started.
static std::wstring g_dataDir;

// Mutable runtime identity/peer, owned by the control-file poller. The
// initial values come from the cfg (+ auto election); the control file may
// change them later.
struct Runtime {
    std::mutex  mtx;
    std::string instance;   // "a" | "b"
    std::string peerIp;
    int         peerPort = 0;
};
static Runtime g_rt;

// Tail target. TailThread re-reads this every pass; bumping the generation
// makes it treat the new file like a fresh start (skip history).
static std::mutex   g_tailMtx;
static std::wstring g_tailPath;
static unsigned     g_tailGen = 0;
static bool         g_tailFixed = false;   // cfg tail_file= override: never retarget

static void SetTailPath(const std::wstring& p)
{
    std::lock_guard<std::mutex> lk(g_tailMtx);
    if (g_tailPath == p) return;
    g_tailPath = p;
    g_tailGen++;
}

static std::wstring CapturePathFor(const std::string& inst)
{
    return g_dataDir + L"tpf2_capture_" + std::wstring(inst.begin(), inst.end()) + L".txt";
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

// cfg lookup order: every candidate name in CFGDIR (the DLL's own directory,
// where the installer drops tpf2_bridge_mp.cfg), then the same names in
// DATADIR (a user override that survives reinstalls). `dataDir` carries a
// trailing backslash (datadir.h contract).
static void LoadConfig(const wchar_t* dllPath, const wchar_t* dataDir, Config& cfg)
{
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, dllPath);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    const std::wstring dirs[] = { std::wstring(dir) + L"\\", std::wstring(dataDir) };

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
    const wchar_t* usedDir = L"";
    for (int d = 0; d < 2 && !f; ++d) {
        for (const std::string& c : candidates) {
            swprintf(path, MAX_PATH, L"%s%S", dirs[d].c_str(), c.c_str());
            _wfopen_s(&f, path, L"rb");
            if (f) { used = c; usedDir = d == 0 ? L"cfg dir" : L"data dir"; break; }
        }
    }
    if (!f) {
        Log("[cfg] NO CFG FOUND in cfg dir or data dir -- using built-in defaults "
            "(inst=%s local=%d)\n", cfg.instance.c_str(), cfg.localPort);
        return;
    }
    Log("[cfg] loaded %s from %S\n", used.c_str(), usedDir);
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

// identity file: the (single) bridge mod reads this to learn which instance
// it is. Lives in DATADIR -- inside a sandbox this lands in the overlay,
// which is exactly the view the mod shares. `warnMismatch` = complain if the
// file already names a different instance (only meaningful at startup; a
// control-file re-identify differs by definition).
static void WriteIdentity(const std::string& inst, bool warnMismatch)
{
    std::wstring idPath = g_dataDir + L"tpf2_instance.txt";

    // If this file already names a DIFFERENT instance, we are almost
    // certainly injected into the wrong process (the a/b DLLs share a
    // directory, and the mod latches identity from here). Say so loudly:
    // a silent overwrite points the other instance at the wrong pair of
    // capture/event files and looks exactly like "the bridge is dead".
    if (warnMismatch) {
        char prev[64] = {0};
        HANDLE ph = CreateFileW(idPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ph != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            ReadFile(ph, prev, sizeof(prev) - 1, &got, nullptr);
            CloseHandle(ph);
            char* nl = strpbrk(prev, "\r\n");
            if (nl) *nl = 0;
            if (prev[0] && inst != prev) {
                Log("[m5] WARNING: identity file said '%s', now claiming '%s' "
                    "-- wrong process? (pid %lu)\n",
                    prev, inst.c_str(), GetCurrentProcessId());
            }
        }
    }

    HANDLE ih = CreateFileW(idPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ih != INVALID_HANDLE_VALUE) {
        // line 1 = instance (all the mod reads); line 2 = owning pid, so a
        // mixed-up injection is diagnosable after the fact; line 3 = the UDP
        // port the bridge actually bound, which the lobby reads to know where
        // to deliver relayed peer frames. Line 3 is only present once the
        // socket is up (Net_LocalPort() == 0 before Net_Init). The mod and the
        // slice read lines 1-2 only, so those two stay byte-identical.
        char buf[128];
        int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s\npid=%lu\n",
                            inst.c_str(), GetCurrentProcessId());
        if (n < 0) n = 0;
        uint16_t port = Net_LocalPort();
        if (port) {
            int m = _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "port=%u\n", port);
            if (m > 0) n += m;
        }
        DWORD written;
        WriteFile(ih, buf, (DWORD)n, &written, nullptr);
        CloseHandle(ih);
    } else {
        Log("[m5] identity file write FAILED (%lu)\n", GetLastError());
    }
}

// events file (peer -> Lua) for the given instance; truncated on open so stale
// events from previous runs don't replay (they already happened once). Any
// previously open events file is closed first.
static void OpenEventsFile(const std::string& inst)
{
    std::wstring evPath = g_dataDir + L"tpf2_events_"
        + std::wstring(inst.begin(), inst.end()) + L".txt";
    FILE* nf = nullptr;
    HANDLE eh = CreateFileW(evPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (eh != INVALID_HANDLE_VALUE) {
        // _O_BINARY, not _O_TEXT: the Lua side tracks a byte offset into this
        // file, and CRLF translation made every line one byte shorter on read
        // than on disk, so the offset drifted backwards and replayed garbage.
        int fd = _open_osfhandle((intptr_t)eh, _O_RDWR | _O_BINARY);
        if (fd >= 0) nf = _fdopen(fd, "r+b");
        if (!nf) CloseHandle(eh);
    }
    {
        std::lock_guard<std::mutex> lk(g_eventsMtx);
        if (g_events) fclose(g_events);
        g_events = nf;
    }
    Log("[m5] events file tpf2_events_%s.txt: %s\n", inst.c_str(),
        nf ? "open (truncated)" : "FAILED");
}

// tail thread: forward new lines from the capture file to the peer. The path
// comes from g_tailPath; a generation bump (re-identify) restarts the tail on
// the new file, skipping whatever history it already holds.
static DWORD WINAPI TailThread(LPVOID)
{
    std::wstring capPath;
    unsigned gen = ~0u;
    uint64_t offset = 0;
    bool started = false;
    std::string line;
    for (;;) {
        Sleep(25);
        {
            std::lock_guard<std::mutex> lk(g_tailMtx);
            if (gen != g_tailGen) {
                gen = g_tailGen;
                capPath = g_tailPath;
                offset = 0;
                started = false;
                Log("[tail] target: %S\n", capPath.c_str());
            }
        }
        if (capPath.empty()) continue;
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

// ---- runtime control file ---------------------------------------------------
// DATADIR\tpf2_bridge_ctl.txt, written by the lobby once roles are decided.
// Whole-file read; a partially written file just parses to fewer lines and the
// remainder is picked up on the next poll.
static bool ReadSmallFile(const std::wstring& path, std::string& out)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    bool ok = ReadFile(h, buf, sizeof(buf), &got, nullptr) != 0;
    CloseHandle(h);
    if (ok) out.assign(buf, got);
    return ok;
}

// Switch letters at run time. Order matters for the Lua contract (it re-reads
// the identity file every 60 ticks and then swaps its own capture/events
// paths): the new events file must exist before the identity flips, and the
// tail must be on the new capture file before the Lua starts writing to it.
static void Reidentify(const std::string& inst)
{
    std::string old;
    {
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        old = g_rt.instance;
        g_rt.instance = inst;
    }
    Log("[ctl] instance %s -> %s: re-identifying (pid %lu)\n",
        old.c_str(), inst.c_str(), GetCurrentProcessId());
    OpenEventsFile(inst);
    if (!g_tailFixed) SetTailPath(CapturePathFor(inst));
    else Log("[ctl] tail_file= override in effect, tail not retargeted\n");
    WriteIdentity(inst, false);
    Log("[ctl] now instance %s (identity rewritten, events truncated, tail -> capture_%s)\n",
        inst.c_str(), inst.c_str());
}

static void ApplyControl(const std::string& text)
{
    std::string wantInst, wantIp;
    int wantPort = 0;
    bool havePeer = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string ln = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ' || ln.back() == '\t')) ln.pop_back();
        if (ln.empty() || ln[0] == '#') continue;
        char ip[64] = {0};
        int port = 0;
        unsigned long ctlPid = 0;
        if (ln == "instance=a" || ln == "instance=b") {
            wantInst = ln.substr(9);
        } else if (sscanf(ln.c_str(), "peer=%63[0-9.]:%d", ip, &port) == 2) {
            wantIp = ip; wantPort = port; havePeer = true;
        } else if (sscanf(ln.c_str(), "pid=%lu", &ctlPid) == 1) {
            // Addressed to a specific bridge: a second instance sharing this
            // data dir (sandbox read-through) must not apply our role.
            if (ctlPid != 0 && ctlPid != GetCurrentProcessId()) {
                Log("[ctl] control file is for pid %lu, not us (%lu) -- ignored\n", ctlPid, GetCurrentProcessId());
                return;
            }
        } else {
            Log("[ctl] ignored line: %.100s\n", ln.c_str());
        }
    }

    if (!wantInst.empty()) {
        bool differs;
        {
            std::lock_guard<std::mutex> lk(g_rt.mtx);
            differs = wantInst != g_rt.instance;
        }
        if (differs) Reidentify(wantInst);
    }
    if (havePeer) {
        bool differs;
        std::string oldIp; int oldPort;
        {
            std::lock_guard<std::mutex> lk(g_rt.mtx);
            oldIp = g_rt.peerIp; oldPort = g_rt.peerPort;
            differs = wantIp != g_rt.peerIp || wantPort != g_rt.peerPort;
        }
        if (differs) {
            if (Net_SetPeer(wantIp.c_str(), wantPort)) {
                std::lock_guard<std::mutex> lk(g_rt.mtx);
                g_rt.peerIp = wantIp; g_rt.peerPort = wantPort;
                Log("[ctl] peer %s:%d -> %s:%d\n", RedactIp(oldIp.c_str()), oldPort,
                    RedactIp(wantIp.c_str()), wantPort);
            } else {
                Log("[ctl] peer=%s:%d REJECTED (not a dotted IPv4:port), keeping %s:%d\n",
                    RedactIp(wantIp.c_str()), wantPort, RedactIp(oldIp.c_str()), oldPort);
            }
        }
    }
}

static DWORD WINAPI CtlThread(LPVOID)
{
    const std::wstring path = g_dataDir + L"tpf2_bridge_ctl.txt";
    std::string last, cur;
    for (;;) {
        Sleep(500);
        if (!ReadSmallFile(path, cur)) cur.clear();   // missing = nothing requested
        if (cur == last) continue;
        last = cur;
        Log("[ctl] control file changed (%zu b)\n", cur.size());
        ApplyControl(cur);
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

    // CFGDIR: where this DLL (and the installer's cfg) lives. Read-only use.
    wchar_t cfgDir[MAX_PATH];
    wcscpy_s(cfgDir, dllPath);
    wchar_t* slash = wcsrchr(cfgDir, L'\\');
    if (slash) *slash = 0;

    // DATADIR: everything written at run time. Trailing backslash included.
    wchar_t dataDir[MAX_PATH] = L"";
    if (!Tpf2mpDataDirW(dataDir, MAX_PATH, (const void*)&InitThread)) {
        // datadir.h only fails if even the module path is unreadable; fall
        // back to CFGDIR so the bridge at least keeps its old behaviour.
        swprintf(dataDir, MAX_PATH, L"%s\\", cfgDir);
    }
    g_dataDir = dataDir;

    // Both instances can share this directory (the alut proxy loads the same
    // bridge into both games), and fopen("a") on Windows is seek-then-write,
    // not an atomic append -- two processes silently overwrite each other's
    // lines. FILE_APPEND_DATA appends atomically, so interleaving is safe.
    wchar_t logPath[MAX_PATH];
    swprintf(logPath, MAX_PATH, L"%stpf2_bridge.log", dataDir);
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
    LoadConfig(dllPath, dataDir, cfg);
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
        cfg.instance.c_str(), cfg.localPort, RedactIp(cfg.peerIp), cfg.peerPort,
        GetCurrentProcessId());
    // The Lua and slice halves must resolve the same data dir (datadir.h /
    // TPF2MP_DATADIR / LOCALAPPDATA), otherwise the halves talk past each
    // other. Log both dirs so a mismatch is obvious.
    Log("[m5] cfg dir: %S data dir: %S\n", cfgDir, dataDir);

    {
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        g_rt.instance = cfg.instance;
        g_rt.peerIp   = cfg.peerIp;
        g_rt.peerPort = cfg.peerPort;
    }

    // A control file left over from a previous session must not be replayed
    // into this one (its instance letter may contradict the election we just
    // did). Consume it: anything that appears from now on is a real request.
    {
        std::wstring ctl = g_dataDir + L"tpf2_bridge_ctl.txt";
        std::string stale;
        if (ReadSmallFile(ctl, stale)) {
            Log("[ctl] removing stale control file (%zu b) from previous session\n", stale.size());
            if (!DeleteFileW(ctl.c_str()))
                Log("[ctl] WARNING: could not delete stale control file (%lu)\n", GetLastError());
        }
    }

    // (identity file is written after Net_Init below, so it can carry the
    // port the socket really bound)
    OpenEventsFile(cfg.instance);

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

    // Identity goes out now that the bound port is known: the lobby reads
    // line 3 (port=) to learn where to hand relayed peer frames to.
    WriteIdentity(cfg.instance, true);
    Log("[m5] identity written: inst=%s port=%u\n", cfg.instance.c_str(),
        (unsigned)Net_LocalPort());

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
        // Path is latched by the hook at install; a later re-identify does
        // not move it (the probe is a dev diagnostic, not a replication path).
        wchar_t buyPath[MAX_PATH];
        swprintf(buyPath, MAX_PATH, L"%stpf2_buy_%S.txt", dataDir, cfg.instance.c_str());
        BuyHook_Install(Log, buyPath);
        Log("[buy] purchases -> %S\n", buyPath);
    } else {
        Log("[buy] disabled (buy_hook=0)\n");
    }

    // capture file tail (Lua -> peer). tail_file= is an absolute override
    // (relay mode) and is never retargeted by the control file.
    if (!cfg.tailFile.empty()) {
        g_tailFixed = true;
        SetTailPath(std::wstring(cfg.tailFile.begin(), cfg.tailFile.end()));
    } else {
        SetTailPath(CapturePathFor(cfg.instance));
    }
    CreateThread(nullptr, 0, TailThread, nullptr, 0, nullptr);
    Log("[m5] tailing capture file\n");

    // runtime control file poller (instance= / peer= changes from the lobby)
    CreateThread(nullptr, 0, CtlThread, nullptr, 0, nullptr);
    Log("[ctl] polling %Stpf2_bridge_ctl.txt every 500 ms\n", dataDir);
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
