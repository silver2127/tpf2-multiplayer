// M5 bridge DLL: file<->UDP bridge, plus the fractional-speed hook.
//   - tails  tpf2_capture_<inst>.txt  (Lua writes local build events here)
//   - sends new lines via reliable UDP to the peer
//   - writes received lines to tpf2_events_<inst>.txt (Lua replays from here)
// No config file: everything is built in, so nothing on disk can change how
// the bridge behaves. The letter is elected from UDP ports 7771/7772 and the
// peer starts at 127.0.0.1; the lobby's control file retargets both.
//
// Every file written at run time -- log, identity, events, captures, control
// file -- lives in DATADIR = Tpf2mpDataDirW (%LOCALAPPDATA%\tpf2mp\data, or
// TPF2MP_DATADIR).
//
// Runtime control: DATADIR\tpf2_bridge_ctl.txt, polled every 500 ms, lines
//   instance=a|b        re-identify (rewrite identity, new events file, retarget tail)
//   peer=<ipv4>:<port>  repoint the UDP peer without restarting the socket
// Identity: DATADIR\tpf2_instance.txt, lines '<a|b>', 'pid=<pid>',
// 'port=<bound UDP port>' (line 3 is what the lobby routes peer frames to).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <mutex>
#include <fcntl.h>
#include <io.h>
#include "net.h"
#include "datadir.h"
#include "speedhook.h"
#include "setplayer_patch.h"

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
    thread_local char buf[4][64];   // Log runs on the tail, ctl and init threads
    thread_local int slot = 0;
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

// Set once, from DllMain(DLL_PROCESS_DETACH). Our worker threads were plain
// for(;;) loops with no way out, so while shutdown ran they kept tailing a file
// and pushing lines at a socket that was being closed underneath them. Nothing
// waits on them (that would need the loader lock we are holding); this only
// stops them doing more work.
static volatile bool g_stopping = false;

// Mutable runtime identity/peer, owned by the control-file poller. The
// initial values come from the port election; the control file may change
// them later.
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

// Startup identity and endpoints. Built in, never read from a file: InitThread
// elects the letter and the port pair, and the peer stays loopback until the
// lobby's control file names another.
struct Config {
    uint16_t localPort = 0;
    char peerIp[64] = "127.0.0.1";
    uint16_t peerPort = 0;
    std::string instance;
};

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
    while (!g_stopping) {
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
        while (!g_stopping) {
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
            Net_QueueLine(line.c_str());
            Log("[tail] sent (%zu b): %.200s\n", line.size(), line.c_str());
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
    SetTailPath(CapturePathFor(inst));
    WriteIdentity(inst, false);
    Log("[ctl] now instance %s (identity rewritten, events truncated, tail -> capture_%s)\n",
        inst.c_str(), inst.c_str());
}

// ---- a second launch of the game in the same box (2026-09-11) ----
// Launching the game inside a Sandboxie box starts TWO game processes: the one
// launched, which hands off to the box's Steam and exits, and Steam's relaunch.
// Both load this bridge and both write tpf2_instance.txt, so whichever writes
// last owns it. When that is the short-lived one, the file names a dead pid,
// the lobby addresses the control file to that pid, and ApplyControl ignored it
// as "not us": the live game never got its letter or its peer and stayed
// disconnected ("one instance lost connection", twice on the four-game rig).
// So: an identity file naming a pid that has EXITED is rewritten for us, that
// pid is remembered as a sibling, and a control file addressed to a sibling
// that has exited is ours. A pid that is still running is never overridden.
static bool PidAlive(unsigned long pid)
{
    if (pid == 0) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    // Access denied means it exists but is not ours to open (another box,
    // another user): alive, so it is left alone.
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

static unsigned long g_siblingPids[16];
static int g_nSiblings = 0;
static volatile unsigned long g_ctlIgnoredPid = 0;   // set when a control file was left for a live pid

static bool IsSibling(unsigned long pid)
{
    for (int i = 0; i < g_nSiblings; i++) if (g_siblingPids[i] == pid) return true;
    return false;
}

// Returns true when it rewrote the identity file (the caller then re-reads the
// control file, which may have been addressed to that sibling).
static bool HealIdentity()
{
    std::string text;
    if (!ReadSmallFile(g_dataDir + L"tpf2_instance.txt", text)) return false;
    const char* p = strstr(text.c_str(), "pid=");
    unsigned long named = 0;
    if (!p || sscanf(p, "pid=%lu", &named) != 1) return false;
    const unsigned long mine = GetCurrentProcessId();
    if (named == 0 || named == mine || PidAlive(named)) return false;
    if (!IsSibling(named) && g_nSiblings < 16) g_siblingPids[g_nSiblings++] = named;
    std::string inst;
    {
        std::lock_guard<std::mutex> lk(g_rt.mtx);
        inst = g_rt.instance;
    }
    Log("[m5] identity file named pid %lu, which has exited (a second launch of the game in this box) "
        "-- rewriting it for us (pid %lu, instance %s)\n", named, mine, inst.c_str());
    WriteIdentity(inst, false);
    return true;
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
        if ((ln.size() == 10 || ln.size() == 11) && ln.rfind("instance=", 0) == 0 && ln[9] >= 'a' && ln[9] <= 'z'
            && (ln.size() == 10 || (ln[10] >= 'a' && ln[10] <= 'z'))) {   // a..z, then aa..: up to 702 players
            wantInst = ln.substr(9);
        } else if (sscanf(ln.c_str(), "peer=%63[0-9.]:%d", ip, &port) == 2) {
            wantIp = ip; wantPort = port; havePeer = true;
        } else if (sscanf(ln.c_str(), "pid=%lu", &ctlPid) == 1) {
            // Addressed to a specific bridge: a second instance sharing this
            // data dir (sandbox read-through) must not apply our role.
            if (ctlPid != 0 && ctlPid != GetCurrentProcessId()) {
                // A sibling launch in this box that has since exited was
                // addressed from the identity file it overwrote: ours.
                // Anything else -- a live pid, or a pid this bridge never saw
                // in its own identity file -- stays someone else's.
                if (!IsSibling(ctlPid) || PidAlive(ctlPid)) {
                    Log("[ctl] control file is for pid %lu, not us (%lu) -- ignored\n", ctlPid, GetCurrentProcessId());
                    g_ctlIgnoredPid = ctlPid;
                    return;
                }
                Log("[ctl] control file is for pid %lu, a sibling launch that has exited -- applying it to us (%lu)\n",
                    ctlPid, GetCurrentProcessId());
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
                Log("[ctl] peer=%s:%d REJECTED (not a dotted IPv4:port, or not this PC while the socket is loopback-only), keeping %s:%d\n",
                    RedactIp(wantIp.c_str()), wantPort, RedactIp(oldIp.c_str()), oldPort);
            }
        }
    }
}

static DWORD WINAPI CtlThread(LPVOID)
{
    const std::wstring path = g_dataDir + L"tpf2_bridge_ctl.txt";
    // DATADIR\tpf2_speed.txt: one number, the fractional speed target (2.5,
    // 0.5, ...). Absent, empty or 0 = off (the engine's own speed). Written by
    // the Lua / the panel; read here so the sim thread never touches a file.
    const std::wstring speedPath = g_dataDir + L"tpf2_speed.txt";
    // A target left behind by a previous session would dither THIS game from
    // its first frame (seen 2026-09-09: a stale 0.5 made speed 4 slower than 1).
    if (DeleteFileW(speedPath.c_str())) Log("[speed] removed a stale tpf2_speed.txt from a previous session\n");
    std::string last, cur, lastSpeed, curSpeed;
    while (!g_stopping) {
        Sleep(500);
        if (!ReadSmallFile(speedPath, curSpeed)) curSpeed.clear();
        if (curSpeed != lastSpeed) {
            lastSpeed = curSpeed;
            double t = atof(curSpeed.c_str());
            SpeedHook_SetTarget(t);
            Log("[speed] target -> %.3f (%s)\n", SpeedHook_Target(), curSpeed.empty() ? "file absent/empty: engine speed" : "from tpf2_speed.txt");
        }
        // Heal first: once the identity names us, a control file that was
        // addressed to the exited sibling is read again, not skipped as unchanged.
        if (HealIdentity()) last.clear();
        {
            unsigned long ign = g_ctlIgnoredPid;
            if (ign && IsSibling(ign) && !PidAlive(ign)) { g_ctlIgnoredPid = 0; last.clear(); }
        }
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
    // DATADIR: everything written at run time. Trailing backslash included.
    // Without one (neither TPF2MP_DATADIR nor LOCALAPPDATA is set) the bridge
    // does not start: the slice and the Lua mod resolve the same directory, so
    // a bridge writing its identity anywhere else would talk to nobody.
    wchar_t dataDir[MAX_PATH] = L"";
    if (!Tpf2mpDataDirW(dataDir, MAX_PATH, (const void*)&InitThread)) return 1;
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
    // Auto identity. With the alut proxy the *same* dll loads into both games,
    // so identity can no longer come from which file was injected where --
    // which is just as well, since getting that wrong was the single most
    // confusing failure mode this project has had. Whoever claims the host
    // port first is "a"; the other is "b".
    // The two ports the election picks between, kept in scope: losing the bind
    // below is itself an election result and has to be able to swap them.
    const uint16_t hostPort  = 7771;
    const uint16_t guestPort = 7772;
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

    Log("[m5] bridge init: inst=%s local=%d peer=%s:%d pid=%lu\n",
        cfg.instance.c_str(), cfg.localPort, RedactIp(cfg.peerIp), cfg.peerPort,
        GetCurrentProcessId());
    // The Lua and slice halves must resolve the same data dir (datadir.h /
    // TPF2MP_DATADIR / LOCALAPPDATA), otherwise the halves talk past each
    // other. Log it so a mismatch is obvious.
    Log("[m5] data dir: %S\n", dataDir);

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

    bool netUp = Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerLine);

    // The election is CHECK-then-BIND, and the two halves are seconds apart:
    // Net_PortAvailable said the host port was free, this bind is where we find
    // out whether it still is. Two games launched together both passed the
    // check and both claimed "a" -- and the loser used to KEEP the letter,
    // retry on localPort+10, and go on addressing the guest port. Two hosts, no
    // guest, and nothing crossing the wire. Losing the bind is the election
    // result: take the other letter and the other port pair.
    if (!netUp && cfg.instance == "a") {
        Log("[m5] lost the race for port %u after the availability check said it "
            "was free -- re-electing as instance b\n", cfg.localPort);
        cfg.instance  = "b";
        cfg.localPort = guestPort;
        cfg.peerPort  = hostPort;
        netUp = Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerLine);
        if (netUp) {
            // everything already derived from the old letter has to follow it
            OpenEventsFile(cfg.instance);
            std::lock_guard<std::mutex> lk(g_rt.mtx);
            g_rt.instance = cfg.instance;
            g_rt.peerIp   = cfg.peerIp;
            g_rt.peerPort = cfg.peerPort;
        } else {
            // Neither port would bind, so the re-election proved nothing about
            // who the host is. Put the letter back: a half-applied swap would
            // leave the identity file saying "b" while the events file, the
            // capture tail and g_rt still said "a", which is the mis-homed
            // state WriteIdentity's mismatch warning exists to catch.
            Log("[m5] port %u would not bind either -- reverting to instance a\n",
                cfg.localPort);
            cfg.instance  = "a";
            cfg.localPort = hostPort;
            cfg.peerPort  = guestPort;
        }
    }
    if (!netUp) {
        // Last resort: a private port nobody is addressing. We stay reachable
        // only once the lobby sends peer=, so this is a degraded mode, not a
        // recovery. Scan a range: with four games on one PC the letters a, b
        // and the third box hold 7771, 7772 and 7782, and a single +10 try
        // left the fourth box with NO TRANSPORT at all (2026-09-10).
        const uint16_t base = cfg.localPort;
        for (int k = 1; k <= 20 && !netUp; k++) {
            cfg.localPort = (uint16_t)(base + 10 * k);
            netUp = Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerLine);
        }
        if (!netUp) cfg.localPort = base;
    }
    // "net up" used to be printed unconditionally, directly under the branch
    // that logs Net_Init FAILED -- so the one line anyone greps for said the
    // transport was fine at the exact moment it was not.
    if (netUp) {
        Log("[m5] net up (local %u)\n", (unsigned)Net_LocalPort());
    } else {
        Log("[m5] Net_Init FAILED on every port -- NO TRANSPORT. Nothing will "
            "replicate; the identity file gets no port= line and the lobby "
            "cannot route to us.\n");
    }

    // Identity goes out now that the bound port is known: the lobby reads
    // line 3 (port=) to learn where to hand relayed peer frames to.
    WriteIdentity(cfg.instance, true);
    Log("[m5] identity written: inst=%s port=%u\n", cfg.instance.c_str(),
        (unsigned)Net_LocalPort());

    // Fractional game speed (speedhook.cpp); CtlThread feeds it the target
    // from tpf2_speed.txt.
    SpeedHook_Install(Log);

    // setPlayer on a track, road, node, signal, station or line-less vehicle
    // re-owns it instead of asserting with a crash dump (setplayer_patch.cpp):
    // a company switch calls it on everything the player owns.
    SetPlayerPatch_Install(Log);

    // Transport health, from our own thread every 10 s. A line is written only
    // when a figure moved, so an idle bridge does not repeat itself all session.
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        char last[192] = "";
        while (!g_stopping) {
            Sleep(10000);
            uint64_t dNoPeer = 0, dOverflow = 0, dOversize = 0;
            size_t pending = 0; bool alive = false;
            Net_Stats(&dNoPeer, &dOverflow, &pending, &alive, &dOversize);
            char cur[192];
            _snprintf_s(cur, sizeof(cur), _TRUNCATE,
                "peer=%s pending=%zu dropped=%llu/%llu/%llu strangers=%llu",
                alive ? "up" : "DOWN", pending,
                (unsigned long long)dNoPeer, (unsigned long long)dOverflow,
                (unsigned long long)dOversize,
                (unsigned long long)Net_DroppedStrangers());
            if (strcmp(cur, last) == 0) continue;
            strcpy_s(last, cur);
            Log("[net] %s\n", cur);
        }
        return 0;
    }, nullptr, 0, nullptr);

    // capture file tail (Lua -> peer); a re-identify retargets it
    SetTailPath(CapturePathFor(cfg.instance));
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
        // NOTHING here may block. This runs under the loader lock, and
        // Net_Shutdown's WaitForSingleObject waited on a thread whose own exit
        // takes that same lock to run every other DLL's DLL_THREAD_DETACH: the
        // wait cannot be satisfied until we return, and we do not return until
        // the wait ends. That is a two-second stall on every clean exit and a
        // deadlock whenever the net thread is inside a DLL entry point.
        //
        // Signal instead: the flag stops the tail and control loops, and
        // closing the socket drops the net thread out of select(). We do not
        // join them and we do not call WSACleanup; the process is going away
        // and the kernel reclaims both.
        g_stopping = true;
        Net_SignalShutdown();
    }
    return TRUE;
}
