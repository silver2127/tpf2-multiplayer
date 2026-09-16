// tpf2_bridge_mp.so -- the Linux build of the bridge (bridge_main.cpp).
//   - tails  tpf2_capture_<inst>.txt  (Lua writes local build events here)
//   - sends new lines via reliable UDP to the peer
//   - writes received lines to tpf2_events_<inst>.txt (Lua replays from here)
// plus the letter election, the identity file and the lobby's control file.
//
// Behaviour mirrors bridge_main.cpp rule for rule, and the incidents behind
// each rule are commented there; this file swaps Win32 for POSIX and nothing
// else. The verified Linux code patches live in speedhook_linux.cpp and
// setplayer_linux.cpp; both are installed after the bridge elects its identity.
//
// Loaded by libtpf2mp_boot.so. Every file lives in DATADIR (datadir_linux.h).
//
// Strings and other heap-owning state are leaked on purpose (S() below). The
// worker threads are detached and still running when exit() runs static
// destructors; a destroyed std::string under a live thread would crash the game
// on its way out.
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include "datadir_linux.h"
#include "net.h"
#include "setplayer_patch.h"
#include "speedhook.h"

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

static void SleepMs(unsigned ms) { usleep(ms * 1000); }

// Mask the host part of a public IPv4 for the log (bridge_main.cpp: RedactIp).
static const char* RedactIp(const char* ip)
{
    thread_local char buf[4][64];
    thread_local int slot = 0;
    if (!ip || !*ip) return "";
    static int show = -1;
    if (show < 0) {
        const char* v = getenv("TPF2MP_LOG_IPS");
        show = (v && v[0] == '1') ? 1 : 0;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (show || sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return ip;
    const bool priv = (a == 127) || (a == 10) || (a == 192 && b == 168)
                   || (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31);
    if (priv) return ip;
    slot = (slot + 1) % 4;
    snprintf(buf[slot], sizeof(buf[slot]), "%u.%u.%u.x", a, b, c);
    return buf[slot];
}

struct Runtime {
    std::mutex  mtx;
    std::string instance;
    std::string peerIp;
    int         peerPort = 0;
};

struct State {
    std::string dataDir;          // with trailing slash; set once in InitThread
    Runtime rt;
    std::mutex eventsMtx;         // OnPeerLine (net thread) vs re-identify (ctl thread)
    FILE* events = nullptr;
    std::mutex tailMtx;
    std::string tailPath;
    unsigned tailGen = 0;
    std::string tailEpoch = std::string(32, '0');
    bool tailFromZero = false;
    std::string epochReadyText;
};
static State& S()
{
    static State* s = new State;
    return *s;
}

static std::atomic<bool> g_stopping{false};

static void OnPeerLine(const char* line)
{
    Log("[net] peer (%zu b): %.200s\n", strlen(line), line);
    std::lock_guard<std::mutex> lk(S().eventsMtx);
    if (S().events) {
        // Bare LF: the Lua side seeks by byte offset.
        fprintf(S().events, "%s\n", line);
        fflush(S().events);
    }
}

static void SetTailPath(const std::string& p)
{
    std::lock_guard<std::mutex> lk(S().tailMtx);
    if (S().tailPath == p) return;
    S().tailPath = p;
    S().tailGen++;
}

static std::string CapturePathFor(const std::string& inst)
{
    return S().dataDir + "tpf2_capture_" + inst + ".txt";
}

struct Config {
    uint16_t localPort = 0;
    char peerIp[64] = "127.0.0.1";
    uint16_t peerPort = 0;
    std::string instance;
};

// Whole-file read of a small file; false if it cannot be opened.
static bool ReadSmallFile(const std::string& path, std::string& out)
{
    out.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t got = fread(buf, 1, sizeof(buf), f);
    bool ok = !ferror(f);
    fclose(f);
    if (ok) out.assign(buf, got);
    return ok;
}

// Identity file: line 1 the letter, line 2 our pid, line 3 the bound UDP port
// once the socket is up (bridge_main.cpp: WriteIdentity).
static void WriteIdentity(const std::string& inst, bool warnMismatch)
{
    const std::string idPath = S().dataDir + "tpf2_instance.txt";

    if (warnMismatch) {
        std::string prev;
        if (ReadSmallFile(idPath, prev)) {
            size_t nl = prev.find_first_of("\r\n");
            if (nl != std::string::npos) prev.resize(nl);
            if (!prev.empty() && inst != prev) {
                Log("[m5] WARNING: identity file said '%s', now claiming '%s' "
                    "-- wrong process? (pid %d)\n", prev.c_str(), inst.c_str(), (int)getpid());
            }
        }
    }

    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\npid=%d\n", inst.c_str(), (int)getpid());
    if (n < 0) n = 0;
    uint16_t port = Net_LocalPort();
    if (port) {
        int m = snprintf(buf + n, sizeof(buf) - n, "port=%u\n", port);
        if (m > 0) n += m;
    }
    // Written beside it and renamed over: the mod, the slice and the lobby read
    // this file at any moment, and a rename is atomic where truncate-then-write
    // is not. The pid in the temp name keeps two games sharing DATADIR apart.
    const std::string tmp = idPath + ".tmp" + std::to_string((int)getpid());
    FILE* f = fopen(tmp.c_str(), "wb");
    bool ok = f != nullptr;
    if (f) {
        ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
        ok = (fclose(f) == 0) && ok;
    }
    if (ok) ok = rename(tmp.c_str(), idPath.c_str()) == 0;
    if (!ok) {
        Log("[m5] identity file write FAILED (%s)\n", strerror(errno));
        unlink(tmp.c_str());
    }
}

// Events file (peer -> Lua) for the given instance, truncated on open so stale
// events from a previous run do not replay.
static void OpenEventsFile(const std::string& inst)
{
    const std::string evPath = S().dataDir + "tpf2_events_" + inst + ".txt";
    FILE* nf = fopen(evPath.c_str(), "wb");
    {
        std::lock_guard<std::mutex> lk(S().eventsMtx);
        if (S().events) fclose(S().events);
        S().events = nf;
    }
    Log("[m5] events file tpf2_events_%s.txt: %s\n", inst.c_str(),
        nf ? "open (truncated)" : "FAILED");
}

// Forward new lines from the capture file to the peer. A generation bump
// (re-identify) restarts the tail on the new file, skipping its history.
static void TailThread()
{
    std::string capPath;
    std::string epoch(32, '0');
    unsigned gen = ~0u;
    uint64_t offset = 0;
    bool started = false;
    std::string line;
    while (!g_stopping) {
        SleepMs(25);
        {
            std::lock_guard<std::mutex> lk(S().tailMtx);
            if (gen != S().tailGen) {
                gen = S().tailGen;
                capPath = S().tailPath;
                offset = 0;
                epoch = S().tailEpoch;
                started = S().tailFromZero;
                Log("[tail] target: %s\n", capPath.c_str());
            }
        }
        if (capPath.empty()) continue;
        FILE* f = fopen(capPath.c_str(), "rb");
        if (!f) continue;
        fseeko(f, 0, SEEK_END);
        uint64_t size = (uint64_t)ftello(f);
        if (!started) {
            offset = size;      // skip history on first look
            started = true;
        } else if (size < offset) {
            Log("[tail] source shrank (%llu < %llu), rewinding to 0\n",
                (unsigned long long)size, (unsigned long long)offset);
            offset = 0;
        }
        fseeko(f, (off_t)offset, SEEK_SET);
        while (!g_stopping) {
            off_t lineStart = ftello(f);
            line.clear();
            bool sawNewline = false;
            for (;;) {
                int c = fgetc(f);
                if (c == EOF) break;
                if (c == '\n') { sawNewline = true; break; }
                line.push_back((char)c);
            }
            // partial line (writer mid-flush)? leave it for the next pass
            if (!sawNewline) { offset = (uint64_t)lineStart; break; }
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) { offset = (uint64_t)ftello(f); continue; }
            Net_QueueLine(line.c_str(), epoch.c_str());
            Log("[tail] sent (%zu b): %.200s\n", line.size(), line.c_str());
            offset = (uint64_t)ftello(f);
        }
        fclose(f);
    }
}

// Switch letters at run time. Order matters for the Lua contract: the new
// events file must exist before the identity flips, and the tail must be on the
// new capture file before the Lua starts writing to it.
static void Reidentify(const std::string& inst)
{
    std::string old;
    {
        std::lock_guard<std::mutex> lk(S().rt.mtx);
        old = S().rt.instance;
        S().rt.instance = inst;
    }
    Log("[ctl] instance %s -> %s: re-identifying (pid %d)\n",
        old.c_str(), inst.c_str(), (int)getpid());
    OpenEventsFile(inst);
    SetTailPath(CapturePathFor(inst));
    WriteIdentity(inst, false);
    Log("[ctl] now instance %s (identity rewritten, events truncated, tail -> capture_%s)\n",
        inst.c_str(), inst.c_str());
}

// Identity healing for a sibling launch that exited (bridge_main.cpp: the
// Sandboxie double launch). Kept on Linux: it is harmless when it never fires,
// and the Steam relaunch path can start the game twice here too.
static bool PidAlive(unsigned long pid)
{
    if (pid == 0) return false;
    // EPERM: it exists but belongs to someone else, so it is alive.
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

static unsigned long g_siblingPids[16];
static int g_nSiblings = 0;
static std::atomic<unsigned long> g_ctlIgnoredPid{0};

static bool IsSibling(unsigned long pid)
{
    for (int i = 0; i < g_nSiblings; i++) if (g_siblingPids[i] == pid) return true;
    return false;
}

static bool HealIdentity()
{
    std::string text;
    if (!ReadSmallFile(S().dataDir + "tpf2_instance.txt", text)) return false;
    const char* p = strstr(text.c_str(), "pid=");
    unsigned long named = 0;
    if (!p || sscanf(p, "pid=%lu", &named) != 1) return false;
    const unsigned long mine = (unsigned long)getpid();
    if (named == 0 || named == mine || PidAlive(named)) return false;
    if (!IsSibling(named) && g_nSiblings < 16) g_siblingPids[g_nSiblings++] = named;
    std::string inst;
    {
        std::lock_guard<std::mutex> lk(S().rt.mtx);
        inst = S().rt.instance;
    }
    Log("[m5] identity file named pid %lu, which has exited (a second launch of the game) "
        "-- rewriting it for us (pid %lu, instance %s)\n", named, mine, inst.c_str());
    WriteIdentity(inst, false);
    return true;
}

// The epoch reset runs under the transport reset lock. An already-read tail
// line carries its old epoch and is rejected rather than sent into a new world.
static void PublishEpochReady()
{
    if (S().epochReadyText.empty()) return;
    const auto target = S().dataDir + "tpf2_epoch_ready.txt", temporary = target + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    if (!file) return;
    const bool written = fwrite(S().epochReadyText.data(), 1, S().epochReadyText.size(), file) == S().epochReadyText.size();
    const bool closed = fclose(file) == 0;
    if (written && closed) rename(temporary.c_str(), target.c_str());
}

static void ResetWorldFiles(const char* epoch)
{
    std::string instance;
    { std::lock_guard<std::mutex> lock(S().rt.mtx); instance = S().rt.instance; }
    OpenEventsFile(instance);
    bool ok;
    { std::lock_guard<std::mutex> lock(S().eventsMtx); ok = S().events != nullptr; }
    {
        std::lock_guard<std::mutex> lock(S().tailMtx);
        FILE* file = fopen(S().tailPath.c_str(), "wb");
        if (!file) ok = false; else if (fclose(file) != 0) ok = false;
        S().tailEpoch = epoch; S().tailFromZero = true; ++S().tailGen;
    }
    S().epochReadyText = "epoch=" + std::string(epoch) + "\nok=" + (ok ? "1" : "0")
        + "\npid=" + std::to_string(getpid()) + "\n";
    PublishEpochReady();
    Log("[ctl] world epoch reset, local files %s\n", ok ? "ready" : "FAILED");
}

static void ApplyControl(const std::string& text)
{
    std::string wantInst, wantIp, wantEpoch, wantLobby;
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
            && (ln.size() == 10 || (ln[10] >= 'a' && ln[10] <= 'z'))) {
            wantInst = ln.substr(9);
        } else if (ln.rfind("lobby=", 0) == 0) {
            wantLobby = ln.substr(6);
        } else if (ln.rfind("epoch=", 0) == 0) {
            wantEpoch = ln.substr(6);
        } else if (sscanf(ln.c_str(), "peer=%63[0-9.]:%d", ip, &port) == 2) {
            wantIp = ip; wantPort = port; havePeer = true;
        } else if (sscanf(ln.c_str(), "pid=%lu", &ctlPid) == 1) {
            if (ctlPid != 0 && ctlPid != (unsigned long)getpid()) {
                if (!IsSibling(ctlPid) || PidAlive(ctlPid)) {
                    Log("[ctl] control file is for pid %lu, not us (%d) -- ignored\n", ctlPid, (int)getpid());
                    g_ctlIgnoredPid = ctlPid;
                    return;
                }
                Log("[ctl] control file is for pid %lu, a sibling launch that has exited -- applying it to us (%d)\n",
                    ctlPid, (int)getpid());
            }
        } else {
            Log("[ctl] ignored line: %.100s\n", ln.c_str());
        }
    }

    if (!wantInst.empty()) {
        bool differs;
        {
            std::lock_guard<std::mutex> lk(S().rt.mtx);
            differs = wantInst != S().rt.instance;
        }
        if (differs) Reidentify(wantInst);
    }
    if (!wantLobby.empty() && (!havePeer ||
        !Net_BeginLobby(wantLobby.c_str(), wantIp.c_str(), wantPort, ResetWorldFiles))) {
        Log("[ctl] invalid lobby boundary rejected\n"); return;
    }
    if (!wantEpoch.empty() && !Net_SetWorldEpoch(wantEpoch.c_str(), ResetWorldFiles))
        Log("[ctl] invalid world epoch rejected\n");
    if (havePeer) {
        bool differs;
        std::string oldIp; int oldPort;
        {
            std::lock_guard<std::mutex> lk(S().rt.mtx);
            oldIp = S().rt.peerIp; oldPort = S().rt.peerPort;
            differs = wantIp != S().rt.peerIp || wantPort != S().rt.peerPort;
        }
        if (differs) {
            if (Net_SetPeer(wantIp.c_str(), wantPort)) {
                std::lock_guard<std::mutex> lk(S().rt.mtx);
                S().rt.peerIp = wantIp; S().rt.peerPort = wantPort;
                Log("[ctl] peer %s:%d -> %s:%d\n", RedactIp(oldIp.c_str()), oldPort,
                    RedactIp(wantIp.c_str()), wantPort);
            } else {
                Log("[ctl] peer=%s:%d REJECTED (not a dotted IPv4:port, or not this PC while the socket is loopback-only), keeping %s:%d\n",
                    RedactIp(wantIp.c_str()), wantPort, RedactIp(oldIp.c_str()), oldPort);
            }
        }
    }
}

static void CtlThread()
{
    const std::string path = S().dataDir + "tpf2_bridge_ctl.txt";
    // tpf2_speed.txt: the fractional speed target; a stale one from a previous
    // session must not dither this game from its first frame.
    const std::string speedPath = S().dataDir + "tpf2_speed.txt";
    if (unlink(speedPath.c_str()) == 0) Log("[speed] removed a stale tpf2_speed.txt from a previous session\n");
    std::string last, cur, lastSpeed, curSpeed, epochControl, lastEpoch;
    while (!g_stopping) {
        SleepMs(500);
        PublishEpochReady();
        if (ReadSmallFile(S().dataDir + "tpf2_epoch_request.txt", epochControl) && epochControl != lastEpoch) {
            const auto owner = epochControl.find("pid=");
            unsigned long pid = 0;
            if (owner != std::string::npos) sscanf(epochControl.c_str() + owner, "pid=%lu", &pid);
            if (pid == static_cast<unsigned long>(getpid())) ApplyControl(epochControl);
            lastEpoch = epochControl;
        }
        if (!ReadSmallFile(speedPath, curSpeed)) curSpeed.clear();
        if (curSpeed != lastSpeed) {
            lastSpeed = curSpeed;
            double t = atof(curSpeed.c_str());
            SpeedHook_SetTarget(t);
            Log("[speed] target -> %.3f (%s)\n", SpeedHook_Target(), curSpeed.empty() ? "file absent/empty: engine speed" : "from tpf2_speed.txt");
        }
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
}

static void HealthThread()
{
    char last[192] = "";
    while (!g_stopping) {
        SleepMs(10000);
        uint64_t dNoPeer = 0, dOverflow = 0, dOversize = 0;
        size_t pending = 0; bool alive = false;
        Net_Stats(&dNoPeer, &dOverflow, &pending, &alive, &dOversize);
        char cur[192];
        snprintf(cur, sizeof(cur),
            "peer=%s pending=%zu dropped=%llu/%llu/%llu strangers=%llu",
            alive ? "up" : "DOWN", pending,
            (unsigned long long)dNoPeer, (unsigned long long)dOverflow,
            (unsigned long long)dOversize,
            (unsigned long long)Net_DroppedStrangers());
        if (strcmp(cur, last) == 0) continue;
        snprintf(last, sizeof(last), "%s", cur);
        Log("[net] %s\n", cur);
    }
}

static void InitThread()
{
    // Without a data dir the bridge does not start: the slice and the Lua mod
    // resolve the same directory, so writing anywhere else talks to nobody.
    char dataDir[4096];
    if (!Tpf2mpDataDirA(dataDir, sizeof(dataDir))) return;
    S().dataDir = dataDir;

    // "a" is O_APPEND: every write lands at the end even with two games
    // appending to the same log (FILE_APPEND_DATA on Windows).
    const std::string logPath = S().dataDir + "tpf2_bridge.log";
    g_log = fopen(logPath.c_str(), "ab");

    Config cfg;
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

    Log("[m5] bridge init: inst=%s local=%d peer=%s:%d pid=%d\n",
        cfg.instance.c_str(), cfg.localPort, RedactIp(cfg.peerIp), cfg.peerPort, (int)getpid());
    Log("[m5] data dir: %s\n", dataDir);

    {
        std::lock_guard<std::mutex> lk(S().rt.mtx);
        S().rt.instance = cfg.instance;
        S().rt.peerIp   = cfg.peerIp;
        S().rt.peerPort = cfg.peerPort;
    }

    // A control file from a previous session must not be replayed into this one.
    {
        const std::string ctl = S().dataDir + "tpf2_bridge_ctl.txt";
        std::string stale;
        if (ReadSmallFile(ctl, stale)) {
            Log("[ctl] removing stale control file (%zu b) from previous session\n", stale.size());
            if (unlink(ctl.c_str()) != 0)
                Log("[ctl] WARNING: could not delete stale control file (%s)\n", strerror(errno));
        }
    }

    OpenEventsFile(cfg.instance);

    bool netUp = Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerLine);

    // Losing the bind after the availability check is itself an election
    // result: take the other letter and port pair.
    if (!netUp && cfg.instance == "a") {
        Log("[m5] lost the race for port %u after the availability check said it "
            "was free -- re-electing as instance b\n", cfg.localPort);
        cfg.instance  = "b";
        cfg.localPort = guestPort;
        cfg.peerPort  = hostPort;
        netUp = Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerLine);
        if (netUp) {
            OpenEventsFile(cfg.instance);
            std::lock_guard<std::mutex> lk(S().rt.mtx);
            S().rt.instance = cfg.instance;
            S().rt.peerIp   = cfg.peerIp;
            S().rt.peerPort = cfg.peerPort;
        } else {
            Log("[m5] port %u would not bind either -- reverting to instance a\n",
                cfg.localPort);
            cfg.instance  = "a";
            cfg.localPort = hostPort;
            cfg.peerPort  = guestPort;
        }
    }
    if (!netUp) {
        // Last resort: a private port nobody is addressing, reachable only once
        // the lobby sends peer=.
        const uint16_t base = cfg.localPort;
        for (int k = 1; k <= 20 && !netUp; k++) {
            cfg.localPort = (uint16_t)(base + 10 * k);
            netUp = Net_Init(cfg.localPort, cfg.peerIp, cfg.peerPort, OnPeerLine);
        }
        if (!netUp) cfg.localPort = base;
    }
    if (netUp) {
        Log("[m5] net up (local %u)\n", (unsigned)Net_LocalPort());
    } else {
        Log("[m5] Net_Init FAILED on every port -- NO TRANSPORT. Nothing will "
            "replicate; the identity file gets no port= line and the lobby "
            "cannot route to us.\n");
    }

    WriteIdentity(cfg.instance, true);
    Log("[m5] identity written: inst=%s port=%u\n", cfg.instance.c_str(),
        (unsigned)Net_LocalPort());

    // TPF2MP_NO_PATCHES=1 in the launch options skips both code patches: the
    // quickest way to tell a crash they cause from anything else.
    const char* noPatches = getenv("TPF2MP_NO_PATCHES");
    if (noPatches && noPatches[0] == '1') {
        Log("[m5] TPF2MP_NO_PATCHES=1: speed hook and setPlayer patch skipped\n");
    } else {
        SpeedHook_Install(Log);
        SetPlayerPatch_Install(Log);
    }

    std::thread(HealthThread).detach();

    SetTailPath(CapturePathFor(cfg.instance));
    std::thread(TailThread).detach();
    Log("[m5] tailing capture file\n");

    std::thread(CtlThread).detach();
    Log("[ctl] polling %stpf2_bridge_ctl.txt every 500 ms\n", dataDir);
}

#ifndef TPF2MP_BRIDGE_TEST
__attribute__((constructor))
static void BridgeLoad()
{
    std::thread(InitThread).detach();
}

// Never blocks (bridge_main.cpp: DLL_PROCESS_DETACH): stop the loops and wake
// the net thread out of select(); the process is going away.
__attribute__((destructor))
static void BridgeUnload()
{
    g_stopping = true;
    Net_SignalShutdown();
}
#endif
