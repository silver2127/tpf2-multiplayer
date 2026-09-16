// lobby_linux.cpp -- the lobby on Linux (lobby_linux.h). The port of
// menu_hook.cpp's EnsureLobbyJob, LobbyThread, StartLobby, LeaveLobby,
// TeardownLobby, QuitLobbyProc, applyRoster, writeBridgeCtl, readBridgePort /
// readBridgePid, pickRelayPort, originLetterFor, writeCompanyCfg,
// speedFromChat, SyncStart / SyncPoll, PubFetchThread and CollectLogsThread.
// The rules, and the incidents behind them, are commented there; this file
// notes what Linux changes. The lobby program's side of the contract is
// docs/linux/NETPUNCH.md (section 3).
//
// THE PROCESS. Windows runs netpunch.exe in a job object with
// KILL_ON_JOB_CLOSE, so the lobby -- and the PyInstaller child that really owns
// udp/29471 -- dies with the game however the game ends. Linux has no job
// objects. What stands in for one:
//   - the lobby gets its own session, so it leads its own process group:
//     stopping it signals the group, the onefile bootloader and the child it
//     started together (TerminateJobObject). Quit first, then SIGTERM, which
//     lobby.py treats as a leave and the bootloader passes on before it removes
//     its /tmp/_MEI* folder, then SIGKILL (NETPUNCH.md 3.4, measured there);
//   - PR_SET_PDEATHSIG(SIGTERM), for a game that crashes or is killed and runs
//     no cleanup at all. The kernel sends it when the THREAD that created the
//     child ends, so the lobby is only ever started from the lobby thread,
//     which lives as long as the process. The bootloader passes the signal on;
//   - --parent-pid <our pid>: lobby.py's own watch of the game (NETPUNCH.md 3.5).
// prctl has to run inside the child, which posix_spawn cannot do, and the game
// is heavily multi-threaded, so fork is out (it copies the address space with
// other threads' locks held mid-use). The child is made the way glibc's own
// posix_spawn makes it: clone(CLONE_VM | CLONE_VFORK) onto a private stack,
// every signal blocked across the call, the handlers the game installed reset
// before the mask comes back, and nothing but system calls until execve.
//
// SIGCHLD. The TransportFever2 image leaves it alone: the image's only callers
// of sigaction and signal are one crash-handler family (0x36c33d0, 0x36c37a0,
// 0x36c38d0, 0x36c42d0), which walks the table at .rodata 0x50637d0
// = { 11, 6, 8, 4, 7, 5 } (SEGV ABRT FPE ILL BUS TRAP; 0x36c37d3 lea rbx, then
// 4-byte steps against 0x98-byte struct sigaction slots from .bss 0x5b3f820).
// popen (0x990362, Lua's io.popen) and system (0x322d43b, 0x322d5be, 0x322d6de)
// are glibc's and reap their own children. That is the image only: the game's
// libSDL2 2.25.0 imports sigaction, signal and waitpid as well (readelf
// --dyn-syms; INFERRED from SDL's sources: SDL_QuitInit's SIGINT / SIGTERM
// handlers and waitpid on pids of its own), and the Steam overlay's preload was
// not checked. Nothing here depends on the claim: a child reaped elsewhere makes
// waitid fail with ECHILD, which HasExited reports as Gone, and once it is Gone
// the lobby is never signalled again (SweepAndReap).
//
// FILES, as on Windows: lobby_out.jsonl (the lobby's events, tailed here),
// lobby_in.jsonl (our commands, appended) and a joiner's incoming_save.* live in
// the lobby folder, $XDG_DATA_HOME/tpf2mp/netpunch (the Snap's HOME applies):
// the first place the Lua mod's CM.netDir looks, passed to the lobby as its
// working directory and --io-dir. The program is found there first, then in
// <game>/netpunch.
//
// THE GAME. Placing the shared save, loading it in-process and the forced
// autosave behind hot join are the menu-game area's (menu_game_linux.h).
// Where menu_hook.cpp reads its own CGameUI capture to tell that a game is
// running, the lobby is told (OnMenuPage, OnGameUiFrame); until something calls
// OnGameUiFrame, hot join and the relay leader's periodic upload never fire
// (logged once).
//
// NUMBERS. The game calls setlocale (an import of the binary), so nothing that
// crosses to another program goes through atof, strtod or %g: JSON numbers and
// the /speed value are read and written by hand.
//
// Heap state is leaked on purpose (S()): the threads are still running when
// exit() runs static destructors.
#include "lobby_linux.h"
#include "datadir_linux.h"
#include "logarchive_linux.h"
#include "menu_game_linux.h"
#include "slice_ready_linux.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace lobby {

static const int GAME_RELAY_PORT_HOST = 7773;   // lobby.py --game-relay-port (menu_hook.cpp)
static const int GAME_RELAY_PORT_JOIN = 7774;
static const int BRIDGE_PORT_DEFAULT  = 7771;   // tpf2_instance.txt without a port= line
static const int MAX_PLAYERS = 200, MAX_COMPANIES = 200;   // lobby.py CAP / MAX_COMPANIES
static const size_t CHAT_LINES = 14;
static const int QUIT_WAIT_MS = 1500;           // menu_hook.cpp TeardownLobby
static const int TERM_WAIT_MS = 2000;           // NETPUNCH.md 3.4
static const uint64_t PUB_EVERY_MS = 10000;     // a third of the master's 30 s TTL
static const int PUB_ROWS = 8;
static const int PUB_TIMEOUT_MS = 20000;        // the program's own GET gives up after 5 s (name lookup aside)
// What chat, START GAME, PUBLIC and the mods answer say to a dead lobby
// (Model::dead), in place of "Lobby is starting...", which would replace the
// reason the status line gave.
static const char kNotRunning[] = "The lobby is not running -- press LEAVE, then HOST or JOIN again.";

// ---- state ------------------------------------------------------------------------------
// The lobby as its events describe it. Everything the panel reads is here,
// behind State::mtx.
struct Model {
    uint64_t gen = 0;          // bumped by Start and Leave: an older lobby's events are dropped
    bool active = false;       // started and not left
    bool dead = false;         // its program is missing, failed to start or exited by itself
    bool isHost = false;       // g_isHost: set by Start, moved by a relay roster
    bool lobbyReady = false;   // the first event arrived: lobby.py has truncated lobby_in.jsonl
    bool saveReady = false;    // joiner: save_ready this session
    bool lobbyDone = false;    // the shared save is placed
    bool relay = false;
    bool separateCompanies = false;
    bool haveCode = false;
    std::string code, you, host, title, modsPrompt;
    std::string xfer;          // xfer= for the in-game window
    std::string speedReq;      // speed= from "/speed"
    int syncReq = 0;           // sync= from "/sync"
    std::string startSave;     // host: the save START GAME shared
    std::vector<std::string> players, stages;
    std::vector<int> companies;
    std::vector<std::string> letters;   // relay lobbies: the relay's origin letter per player
    std::deque<std::string> chat;
    long storedAge = -1, storedMax = -1;
    int relayPort = 0;         // this lobby's --game-relay-port
    int lastCount = 0;         // hot join: the previous roster's size
};

struct Request {
    enum Kind { Launch, Stop, Line, ShareStart, LoadedSave } kind;
    uint64_t gen;
    StartRequest start;
    std::string line;
};

struct Child {
    pid_t pid = 0;
    uint64_t gen = 0;
    uint64_t offset = 0;       // bytes of lobby_out.jsonl consumed
    std::string partial;       // an unfinished line
    long lines = 0;
};

struct State {
    Config cfg;
    Tpf2mpLogFn log = nullptr;
    StatusFn status = nullptr;
    DirtyFn dirty = nullptr;

    std::mutex mtx;
    Model m;

    std::mutex qMtx;
    std::condition_variable qCv;
    std::deque<Request> q;

    // lobby thread only
    Child child;
    std::string lobbyDir;      // the running lobby's folder, trailing slash
    std::string ctlLast;
    uint64_t syncBaseline = 0, syncAskedAt = 0, syncLastSize = 0;
    uint64_t unpausedMs = 0, unpausedLast = 0, sharedUnpaused = 0;
    std::string sharedSave, stageSent;
    std::string worldGen;
    bool sessionStarted = false, worldGenHold = false, switchShare = false, hostLoadedItself = false;
    bool stageWatch = false;
    std::string syncSave;
    uint64_t relayLastUp = 0;
    bool loggedNoGameUi = false;

    std::mutex pubMtx;
    std::condition_variable pubCv;
    bool pubWanted = false, pubBusy = false, pubForce = false;
    uint64_t pubLast = 0;
    std::vector<PubRow> pub;
    std::string pubNote;
    int pubLogged = 0;
};

static State& S()
{
    static State* s = new State;
    return *s;
}

static std::atomic<bool> g_inited{false};       // the lobby thread runs: Start may queue
static std::atomic<bool> g_initFailed{false};   // Init ran, but the lobby thread was refused
static std::atomic<bool> g_pubStarted{false};   // the public list's thread runs
static std::atomic<bool> g_autoCopy{false};
static std::atomic<bool> g_captures{true};
static std::atomic<bool> g_titleMenu{false};
static std::atomic<bool> g_gameUiSeen{false};
static std::atomic<bool> g_logsBusy{false};
static std::atomic<pid_t> g_childPid{0};
static char g_quitPath[4096] = "";   // lobby_in.jsonl of the running lobby, for the exit hook

// ---- small things -----------------------------------------------------------------------
static void Log(const char* fmt, ...)
{
    if (!S().log) return;
    char buf[1600];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    S().log("%s", buf);
}

// Never with State::mtx held (the panel takes its own lock in there).
static void Status(const std::string& s) { if (S().status) S().status(s.c_str()); }
static void Dirty() { if (S().dirty) S().dirty(); }

static bool CancellationReady(std::string* why)
{
    const char* detail = nullptr;
    if (SliceHooksReady(S().cfg.dataDir.c_str(), &detail)) return true;
    Log("[lobby] multiplayer start refused: %s\n", detail ? detail : "cancellation hooks unavailable");
    if (why) *why = "Multiplayer command hooks are not ready. See tpf2_slice.log.";
    return false;
}

static uint64_t NowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void SleepMs(unsigned ms) { usleep(ms * 1000); }

static bool Exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }
static bool IsExec(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
}

static std::string NoSlash(const std::string& dir)
{
    return dir.size() > 1 && dir.back() == '/' ? dir.substr(0, dir.size() - 1) : dir;
}

static std::string FindInPath(const char* name)
{
    const char* env = getenv("PATH");
    const std::string all = env && *env ? env : "/usr/local/bin:/usr/bin:/bin";
    size_t pos = 0;
    while (pos <= all.size()) {
        const size_t sep = all.find(':', pos);
        const std::string dir = all.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        pos = sep == std::string::npos ? all.size() + 1 : sep + 1;
        if (dir.empty() || dir[0] != '/') continue;
        const std::string cand = dir + "/" + name;
        if (IsExec(cand)) return cand;
    }
    return std::string();
}

static bool ReadSmallFile(const std::string& path, std::string* out)
{
    out->clear();
    FILE* f = fopen(path.c_str(), "rbe");
    if (!f) return false;
    char buf[4096];
    const size_t got = fread(buf, 1, sizeof(buf), f);
    const bool ok = !ferror(f);
    fclose(f);
    if (ok) out->assign(buf, got);
    return ok;
}

// Beside it, then renamed over: the bridge polls the ctl every 500 ms and the
// game script reads both files at any moment.
static bool WriteFileAtomic(const std::string& path, const std::string& content, std::string* err)
{
    const std::string tmp = path + ".tmp" + std::to_string((int)getpid());
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { *err = "cannot create " + tmp + ": " + strerror(errno); return false; }
    size_t done = 0;
    while (done < content.size()) {
        const ssize_t w = write(fd, content.data() + done, content.size() - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (size_t)w;
    }
    const bool wrote = done == content.size();
    const bool closed = close(fd) == 0;
    if (!wrote || !closed) { *err = "write failed: " + std::string(strerror(errno)); unlink(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path.c_str()) != 0) { *err = "replace failed: " + std::string(strerror(errno)); unlink(tmp.c_str()); return false; }
    return true;
}

static uint64_t MtimeNs(const std::string& path, uint64_t* size)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) { if (size) *size = 0; return 0; }
    if (size) *size = (uint64_t)st.st_size;
    return (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
}

// Whole characters only.
static std::string Cap(const std::string& s, size_t n)
{
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    return s.substr(0, k);
}

// One line for the panel: control characters become spaces.
static std::string OneLine(const std::string& s)
{
    std::string o = s;
    for (char& c : o) if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
    return o;
}

// lobby.py reads lobby_in.jsonl as UTF-8 and drops a line that is not: a
// malformed sequence becomes '?' so the rest of the message still goes.
static std::string ValidUtf8(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    const unsigned char* p = (const unsigned char*)s.data();
    const unsigned char* end = p + s.size();
    static const uint32_t kMin[4] = { 0, 0x80, 0x800, 0x10000 };
    while (p < end) {
        const unsigned char c = *p;
        const int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        bool ok = more >= 0 && end - p > more;
        uint32_t cp = more <= 0 ? c : more == 1 ? (c & 0x1Fu) : more == 2 ? (c & 0x0Fu) : (c & 0x07u);
        for (int i = 1; ok && i <= more; i++) {
            ok = (p[i] & 0xC0) == 0x80;
            cp = (cp << 6) | (p[i] & 0x3Fu);
        }
        // overlong forms, surrogates and code points past U+10FFFF: Python refuses those too
        if (ok && (cp < kMin[more] || (cp >= 0xD800 && cp < 0xE000) || cp > 0x10FFFF)) ok = false;
        if (!ok) { o.push_back('?'); p++; continue; }
        o.append((const char*)p, (size_t)more + 1);
        p += more + 1;
    }
    return o;
}

static std::string JsonEscape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back((char)c); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o.push_back((char)c);
    }
    return o;
}

// A decimal number, by hand (see NUMBERS). Advances p past what it used.
static bool ParseNumber(const char*& p, const char* end, double* out)
{
    const char* q = p;
    bool neg = false;
    if (q < end && (*q == '-' || *q == '+')) { neg = *q == '-'; q++; }
    double v = 0;
    bool digits = false;
    while (q < end && *q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; digits = true; }
    if (q < end && *q == '.') {
        q++;
        double scale = 0.1;
        while (q < end && *q >= '0' && *q <= '9') { v += (*q - '0') * scale; scale *= 0.1; q++; digits = true; }
    }
    if (!digits) return false;
    if (q < end && (*q == 'e' || *q == 'E')) {
        const char* e = q + 1;
        bool eneg = false;
        if (e < end && (*e == '-' || *e == '+')) { eneg = *e == '-'; e++; }
        int ex = 0;
        bool edig = false;
        while (e < end && *e >= '0' && *e <= '9') { if (ex < 400) ex = ex * 10 + (*e - '0'); e++; edig = true; }
        if (edig) {
            for (int i = 0; i < ex; i++) v = eneg ? v / 10 : v * 10;
            q = e;
        }
    }
    *out = neg ? -v : v;
    p = q;
    return true;
}

// "%.4g" for a speed in (0, 64), without the locale: at most four significant
// digits, trailing zeros dropped.
static std::string FormatSpeed(double v)
{
    int decimals = v >= 10 ? 2 : v >= 1 ? 3 : 4;
    for (double x = v; v < 1 && x < 0.1 && decimals < 12; x *= 10) decimals++;
    unsigned long long scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    const unsigned long long n = (unsigned long long)(v * (double)scale + 0.5);
    char buf[48];
    snprintf(buf, sizeof(buf), "%llu.%0*llu", n / scale, decimals, n % scale);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// ---- JSON --------------------------------------------------------------------------------
// The events are json.dumps output: flat objects with a few arrays and maps,
// non-ASCII as \uXXXX. A small tree reader, where menu_hook.cpp searched the
// text for "key": a player named like a key could steer that search.
struct Json {
    enum Type : uint8_t { Null, Bool, Num, Str, Arr, Obj };
    Type type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;          // Arr items, Obj values
    std::vector<std::string> keys;    // Obj keys

    const Json* Get(const char* key) const
    {
        if (type != Obj) return nullptr;
        const Json* found = nullptr;
        for (size_t i = 0; i < keys.size(); i++) if (keys[i] == key) found = &items[i];   // the last, like json.loads
        return found;
    }
};

struct JsonReader {
    const char* p;
    const char* end;
    int depth = 0;

    void Ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; }

    static void PutUtf8(std::string* o, uint32_t cp)
    {
        if (cp < 0x80) o->push_back((char)cp);
        else if (cp < 0x800) { o->push_back((char)(0xC0 | (cp >> 6))); o->push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { o->push_back((char)(0xE0 | (cp >> 12))); o->push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o->push_back((char)(0x80 | (cp & 0x3F))); }
        else { o->push_back((char)(0xF0 | (cp >> 18))); o->push_back((char)(0x80 | ((cp >> 12) & 0x3F))); o->push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o->push_back((char)(0x80 | (cp & 0x3F))); }
    }

    bool Hex4(uint32_t* cp)
    {
        if (end - p < 4) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = p[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        p += 4;
        *cp = v;
        return true;
    }

    bool String(std::string* o)
    {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { o->push_back(c); continue; }
            if (p >= end) return false;
            const char e = *p++;
            switch (e) {
                case '"': case '\\': case '/': o->push_back(e); break;
                case 'b': o->push_back('\b'); break;
                case 'f': o->push_back('\f'); break;
                case 'n': o->push_back('\n'); break;
                case 'r': o->push_back('\r'); break;
                case 't': o->push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        uint32_t lo;
                        const char* save = p;
                        if (end - p >= 6 && p[0] == '\\' && p[1] == 'u' && (p += 2, Hex4(&lo)) && lo >= 0xDC00 && lo < 0xE000)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { p = save; cp = 0xFFFD; }
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        cp = 0xFFFD;
                    }
                    PutUtf8(o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool Value(Json* v)
    {
        Ws();
        if (p >= end || ++depth > 64) return false;
        bool ok = false;
        const char c = *p;
        if (c == '{') {
            v->type = Json::Obj;
            p++;
            Ws();
            if (p < end && *p == '}') { p++; ok = true; }
            else for (;;) {
                Ws();
                std::string k;
                if (!String(&k)) break;
                Ws();
                if (p >= end || *p != ':') break;
                p++;
                v->keys.push_back(std::move(k));
                v->items.emplace_back();
                if (!Value(&v->items.back())) break;
                Ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; ok = true; }
                break;
            }
        } else if (c == '[') {
            v->type = Json::Arr;
            p++;
            Ws();
            if (p < end && *p == ']') { p++; ok = true; }
            else for (;;) {
                v->items.emplace_back();
                if (!Value(&v->items.back())) break;
                Ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; ok = true; }
                break;
            }
        } else if (c == '"') {
            v->type = Json::Str;
            ok = String(&v->s);
        } else if (end - p >= 4 && !memcmp(p, "true", 4)) {
            v->type = Json::Bool; v->b = true; p += 4; ok = true;
        } else if (end - p >= 5 && !memcmp(p, "false", 5)) {
            v->type = Json::Bool; v->b = false; p += 5; ok = true;
        } else if (end - p >= 4 && !memcmp(p, "null", 4)) {
            v->type = Json::Null; p += 4; ok = true;
        } else {
            v->type = Json::Num;
            ok = ParseNumber(p, end, &v->n);
        }
        depth--;
        return ok;
    }
};

static bool ParseJson(const std::string& text, Json* out)
{
    JsonReader r{ text.data(), text.data() + text.size() };
    if (!r.Value(out)) return false;
    r.Ws();
    return r.p == r.end;
}

static std::string JStr(const Json& o, const char* key)
{
    const Json* v = o.Get(key);
    return v && v->type == Json::Str ? v->s : std::string();
}
static bool JBool(const Json& o, const char* key, bool dflt)
{
    const Json* v = o.Get(key);
    return v && v->type == Json::Bool ? v->b : dflt;
}
static long JInt(const Json& o, const char* key, long dflt)
{
    const Json* v = o.Get(key);
    if (!v) return dflt;
    if (v->type == Json::Num) return v->n > 2e9 ? 2000000000L : v->n < -2e9 ? -2000000000L : (long)v->n;
    if (v->type == Json::Bool) return v->b ? 1 : 0;
    return dflt;
}

// ---- starting a process (see THE PROCESS) ---------------------------------------------------
struct SpawnSpec {
    std::vector<std::string> argv;    // argv[0] is the program's path
    std::vector<std::string> env;     // "KEY=value", replacing the game's KEY
    std::string cwd;
    int stdoutFd = -1, stderrFd = -1; // -1: /dev/null
    bool newSession = false;          // setsid: its own session and process group
    int deathSignal = 0;              // PR_SET_PDEATHSIG
};

struct ChildArgs {
    const char* path;
    char* const* argv;
    char* const* envp;
    const char* cwd;
    int in, out, err;
    bool newSession;
    int deathSignal;
    long parent;
    int maxFd;
    unsigned long mask;   // the thread's signal mask, restored right before execve
    int error;            // written by the child when a step fails
};

// The kernel's struct sigaction (x86-64), for rt_sigaction without glibc's
// filtering of its internal signals.
struct KernelSigaction { void* handler; unsigned long flags; void* restorer; unsigned long mask; };

// Runs in the child, on its own stack, in the game's address space until
// execve: system calls only.
static int ChildMain(void* p)
{
    ChildArgs* a = static_cast<ChildArgs*>(p);
    // No handler of the game's may ever run here. glibc's posix_spawn does the
    // same, with its two internal signals (32, 33) set to ignored.
    for (int sig = 1; sig < 65; sig++) {
        if (sig == SIGKILL || sig == SIGSTOP) continue;
        KernelSigaction old = {};
        if (syscall(SYS_rt_sigaction, sig, nullptr, &old, sizeof(old.mask)) != 0) continue;
        if (old.handler == (void*)0 || old.handler == (void*)1) continue;   // SIG_DFL, SIG_IGN
        KernelSigaction fresh = {};
        fresh.handler = (sig == 32 || sig == 33) ? (void*)1 : (void*)0;
        syscall(SYS_rt_sigaction, sig, &fresh, nullptr, sizeof(fresh.mask));
    }
    if (a->deathSignal) {
        if (syscall(SYS_prctl, PR_SET_PDEATHSIG, (unsigned long)a->deathSignal, 0ul, 0ul, 0ul) != 0) { a->error = errno ? errno : EINVAL; _exit(127); }
        if (syscall(SYS_getppid) != a->parent) _exit(127);   // the game went before the line above took hold
    }
    if (a->newSession && syscall(SYS_setsid) < 0) { a->error = errno ? errno : EPERM; _exit(127); }
    if (syscall(SYS_dup2, a->in, 0) < 0 || syscall(SYS_dup2, a->out, 1) < 0 || syscall(SYS_dup2, a->err, 2) < 0) {
        a->error = errno ? errno : EBADF;
        _exit(127);
    }
    // Nothing else of the game's: the bridge's UDP socket, device files, logs.
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
        for (int fd = 3; fd < a->maxFd; fd++) syscall(SYS_close, fd);
    if (a->cwd && a->cwd[0] && syscall(SYS_chdir, a->cwd) != 0) { a->error = errno ? errno : ENOENT; _exit(127); }
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, &a->mask, nullptr, sizeof(a->mask));
    syscall(SYS_execve, a->path, a->argv, a->envp);
    a->error = errno ? errno : ENOEXEC;
    _exit(127);
}

// A descriptor at 3 or above, so dup2 onto 0..2 never meets itself.
static int HighFd(int fd)
{
    if (fd < 0 || fd > 2) return fd;
    const int hi = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    return hi;
}

// The child's pid, or -1 with *err set. The environment is the game's without
// LD_PRELOAD (the loader removed itself already; the Steam overlay has no
// business in a lobby) and with SpawnSpec::env over it.
static pid_t Spawn(const SpawnSpec& sp, int* err)
{
    *err = 0;
    if (sp.argv.empty()) { *err = EINVAL; return -1; }
    std::vector<char*> argv;
    for (const std::string& s : sp.argv) argv.push_back((char*)s.c_str());
    argv.push_back(nullptr);
    std::vector<std::string> envStore;
    for (char** e = environ; e && *e; e++) {
        if (strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
        bool replaced = false;
        for (const std::string& x : sp.env) {
            const size_t eq = x.find('=');
            if (eq != std::string::npos && strncmp(*e, x.c_str(), eq + 1) == 0) { replaced = true; break; }
        }
        if (!replaced) envStore.emplace_back(*e);
    }
    for (const std::string& x : sp.env) envStore.push_back(x);
    std::vector<char*> envp;
    for (std::string& s : envStore) envp.push_back(&s[0]);
    envp.push_back(nullptr);

    const int devnull = HighFd(open("/dev/null", O_RDWR | O_CLOEXEC));
    if (devnull < 0) { *err = errno ? errno : ENOENT; return -1; }
    ChildArgs a = {};
    a.path = argv[0];
    a.argv = argv.data();
    a.envp = envp.data();
    a.cwd = sp.cwd.c_str();
    a.in = devnull;
    a.out = sp.stdoutFd >= 0 ? sp.stdoutFd : devnull;
    a.err = sp.stderrFd >= 0 ? sp.stderrFd : devnull;
    a.newSession = sp.newSession;
    a.deathSignal = sp.deathSignal;
    a.parent = (long)getpid();
    rlimit rl = {};
    a.maxFd = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 65536) ? (int)rl.rlim_cur : 65536;

    const size_t stackSize = 256 * 1024;
    void* stack = mmap(nullptr, stackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) { *err = errno; close(devnull); return -1; }
    const unsigned long all = ~0ul;
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, &a.mask, sizeof(all));
    const pid_t pid = clone(&ChildMain, (char*)stack + stackSize, CLONE_VM | CLONE_VFORK | SIGCHLD, &a);
    const int cloneErr = errno;
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, &a.mask, nullptr, sizeof(a.mask));
    munmap(stack, stackSize);
    close(devnull);
    if (pid < 0) { *err = cloneErr; return -1; }
    if (a.error) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        *err = a.error;
        return -1;
    }
    return pid;
}

enum class Exit { Running, Exited, Gone };

// Exited leaves the child a zombie, so its pid -- and with it its process
// group's id -- cannot be taken by anyone else until Reap.
static Exit HasExited(pid_t pid)
{
    siginfo_t si = {};
    if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) != 0)
        return errno == EINTR ? Exit::Running : Exit::Gone;   // ECHILD: reaped elsewhere
    return si.si_pid == pid ? Exit::Exited : Exit::Running;
}

static int Reap(pid_t pid)
{
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return 0;
    }
    return st;
}

// Kill what the group leader left behind, then reap the leader.
static int SweepAndReap(pid_t pid, Exit e)
{
    if (e != Exit::Gone) kill(-pid, SIGKILL);
    return e == Exit::Gone ? 0 : Reap(pid);
}

static bool WaitExit(pid_t pid, int ms, Exit* e)
{
    for (int t = 0;; t += 20) {
        *e = HasExited(pid);
        if (*e != Exit::Running) return true;
        if (t >= ms) return false;
        SleepMs(20);
    }
}

// SIGTERM to the group, then SIGKILL: the order NETPUNCH.md 3.4 measured.
// Returns how it went for the log.
static const char* StopGroup(pid_t pid, int termWaitMs, int* status)
{
    Exit e = Exit::Running;
    kill(-pid, SIGTERM);
    if (WaitExit(pid, termWaitMs, &e)) {
        *status = SweepAndReap(pid, e);
        return "on SIGTERM";
    }
    kill(-pid, SIGKILL);
    *status = Reap(pid);
    return nullptr;
}

static std::string DescribeStatus(int st)
{
    char b[64];
    if (WIFEXITED(st)) snprintf(b, sizeof(b), "exit code %d", WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) snprintf(b, sizeof(b), "signal %d", WTERMSIG(st));
    else snprintf(b, sizeof(b), "status 0x%x", st);
    return b;
}

// ---- where the lobby program is ---------------------------------------------------------------
struct Program {
    std::vector<std::string> argv0;   // the program, or python3 and the path of lobby.py
    std::string dir;                  // where it was found, trailing slash
};

static std::string XdgNetDir()
{
    char root[4096];
    return Tpf2mpRootDir(root, sizeof(root)) ? std::string(root) + "/netpunch/" : std::string();
}

// menu_hook.cpp resolveNetDir + LobbyThread: the frozen lobby in
// $XDG_DATA_HOME/tpf2mp/netpunch, then <game>/netpunch; `python3 lobby.py` only
// when python3 exists (the Steam runtime's container has none).
static bool ResolveProgram(Program* prog, std::string* tried)
{
    const std::string xdg = XdgNetDir();
    const std::string game = S().cfg.gameDir.empty() ? std::string() : S().cfg.gameDir + "netpunch/";
    tried->clear();
    for (const std::string& d : { xdg, game }) {
        if (d.empty()) continue;
        const std::string exe = d + "netpunch";
        if (IsExec(exe)) { prog->argv0 = { exe }; prog->dir = d; return true; }
        *tried += (tried->empty() ? "" : ", ") + exe + (Exists(exe) ? " (not executable)" : "");
    }
    const std::string py = FindInPath("python3");
    for (const std::string& d : { xdg, game }) {
        if (d.empty() || !Exists(d + "lobby.py")) continue;
        if (py.empty()) { *tried += ", " + d + "lobby.py (no python3)"; continue; }
        prog->argv0 = { py, d + "lobby.py" };
        prog->dir = d;
        return true;
    }
    return false;
}

// The lobby folder: $XDG_DATA_HOME/tpf2mp/netpunch, created when the program
// lives in the game folder. Only without a usable XDG_DATA_HOME or HOME is it
// the program's own folder (the Lua side's ./netpunch then).
static std::string LobbyFolder(const Program& prog)
{
    const std::string xdg = XdgNetDir();
    if (!xdg.empty() && Tpf2mpMkdirs(NoSlash(xdg).c_str())) return xdg;
    return prog.dir;
}

// ---- the files shared with the bridge and the game script ---------------------------------------
// tpf2_instance.txt: <letter>\npid=<pid>\nport=<bound UDP port>\n (bridge_linux.cpp WriteIdentity).
static bool ReadIdentityValue(const char* key, unsigned long* v)
{
    std::string text;
    if (!ReadSmallFile(S().cfg.dataDir + "tpf2_instance.txt", &text)) return false;
    const size_t klen = strlen(key);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        if (nl - pos > klen && text.compare(pos, klen, key) == 0) {
            unsigned long n = 0;
            bool any = false;
            for (size_t i = pos + klen; i < nl && text[i] >= '0' && text[i] <= '9' && n < 100000000ul; i++) { n = n * 10 + (unsigned long)(text[i] - '0'); any = true; }
            if (any && n) { *v = n; return true; }
        }
        pos = nl + 1;
    }
    return false;
}

static int ReadBridgePort()
{
    unsigned long v = 0;
    if (ReadIdentityValue("port=", &v) && v < 65536) return (int)v;
    Log("[lobby] no port= in %stpf2_instance.txt -> bridge port default %d\n", S().cfg.dataDir.c_str(), BRIDGE_PORT_DEFAULT);
    return BRIDGE_PORT_DEFAULT;
}

static unsigned long ReadBridgePid()
{
    unsigned long v = 0;
    return ReadIdentityValue("pid=", &v) ? v : 0;
}

// A joiner takes the first free relay port from 7774 up (menu_hook.cpp
// pickRelayPort). lobby.py binds 127.0.0.1:<port>; a bind to the wildcard
// address fails with EADDRINUSE while any address holds the port.
static bool UdpPortFree(int port)
{
    const int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return true;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    const bool ok = bind(s, (sockaddr*)&a, sizeof(a)) == 0;
    close(s);
    return ok;
}

static int PickRelayPort(bool join)
{
    if (!join) return GAME_RELAY_PORT_HOST;
    for (int p = GAME_RELAY_PORT_JOIN; p < GAME_RELAY_PORT_JOIN + 32; p++)
        if (UdpPortFree(p)) return p;
    return GAME_RELAY_PORT_JOIN;
}

// Origin names: a..z, then aa, ab, ... (menu_hook.cpp originName).
static std::string OriginName(int idx)
{
    if (idx < 0) idx = 0;
    char out[3] = { 0, 0, 0 };
    if (idx < 26) out[0] = (char)('a' + idx);
    else { idx -= 26; out[0] = (char)('a' + (idx / 26) % 26); out[1] = (char)('a' + idx % 26); }
    return out;
}

// The host is 'a', joiners b, c, ... in roster order skipping the host; a relay
// lobby's letters come from the relay (menu_hook.cpp originLetterFor).
static std::string OriginLetterFor(const Model& m, const std::string& name)
{
    if (m.relay)
        for (size_t i = 0; i < m.players.size(); i++)
            if (m.players[i] == name && !m.letters[i].empty()) return m.letters[i];
    if (name == m.host) return "a";
    int idx = 0;
    for (const std::string& p : m.players) {
        if (p == m.host) continue;
        if (p == name) break;
        idx++;
    }
    return OriginName(idx + 1);
}

// The bridge accepts instance=[a-z]{1,2} only (bridge_linux.cpp ApplyControl).
static bool ValidLetter(const std::string& s)
{
    if (s.empty() || s.size() > 2) return false;
    for (char c : s) if (c < 'a' || c > 'z') return false;
    return true;
}

// tpf2_bridge_ctl.txt (menu_hook.cpp writeBridgeCtl): our letter, where the
// bridge sends frames (this lobby's relay port), whose bridge it is for, the
// roster size, and the game script's speed=, sync=, xfer=, leader=.
// Lobby thread.
static void WriteBridgeCtl(bool isHost)
{
    const unsigned long bpid = ReadBridgePid();
    std::string letter = "a", leader = "a", speed, xfer;
    int count, port, sync;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        const Model& m = S().m;
        bool fromRelay = false;
        if (m.relay)
            for (size_t i = 0; i < m.players.size(); i++)
                if (m.players[i] == m.you && !m.letters[i].empty()) { letter = m.letters[i]; fromRelay = true; break; }
        if (!isHost && !fromRelay) {
            int idx = 0;
            for (const std::string& p : m.players) {
                if (p == m.host) continue;
                if (p == m.you) break;
                idx++;
            }
                    letter = OriginName(idx + 1);
        }
        count = (int)m.players.size();
        port = m.relayPort ? m.relayPort : (isHost ? GAME_RELAY_PORT_HOST : GAME_RELAY_PORT_JOIN);
        speed = m.speedReq;
        sync = m.syncReq;
        xfer = m.xfer;
        if (!m.host.empty()) leader = OriginLetterFor(m, m.host);
    }
    std::string content = "instance=" + letter + "\npeer=127.0.0.1:" + std::to_string(port) + "\npid=" + std::to_string(bpid)
                        + "\nplayers=" + std::to_string(count) + "\n";
    if (!speed.empty()) content += "speed=" + speed + "\n";
    if (sync) content += "sync=" + std::to_string(sync) + "\n";
    if (!xfer.empty()) content += "xfer=" + xfer + "\n";
    content += "leader=" + leader + "\n";
    if (content == S().ctlLast) return;
    std::string err;
    if (!WriteFileAtomic(S().cfg.dataDir + "tpf2_bridge_ctl.txt", content, &err)) {
        Log("[lobby] bridge ctl: %s\n", err.c_str());
        return;
    }
    S().ctlLast = content;
    Log("[lobby] bridge ctl -> %stpf2_bridge_ctl.txt: instance=%s peer=127.0.0.1:%d players=%d leader=%s\n",
        S().cfg.dataDir.c_str(), letter.c_str(), port, count, leader.c_str());
}

// mp_company_cfg.txt for the game script (menu_hook.cpp writeCompanyCfg):
//   coop | companies / my company / every distinct id / origin=company map.
static void WriteCompanyCfg()
{
    std::string l3, l4;
    int mine = 1, distinct = 0;
    std::vector<bool> seen((size_t)MAX_COMPANIES + 1, false);
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        const Model& m = S().m;
        for (size_t i = 0; i < m.players.size(); i++) {
            int cid = m.companies[i];
            cid = cid < 1 ? 1 : cid > MAX_COMPANIES ? MAX_COMPANIES : cid;
            if (m.players[i] == m.you) mine = cid;
            if (!seen[(size_t)cid]) { seen[(size_t)cid] = true; distinct++; }
            l4 += (l4.empty() ? "" : ",") + OriginLetterFor(m, m.players[i]) + "=" + std::to_string(cid);
        }
    }
    for (int c = 1; c <= MAX_COMPANIES; c++)
        if (seen[(size_t)c]) l3 += (l3.empty() ? "" : ",") + std::to_string(c);
    const std::string mode = distinct > 1 ? "companies" : "coop";
    const std::string content = mode + "\n" + std::to_string(mine) + "\n" + l3 + "\n" + l4 + "\n";
    std::string err;
    if (!WriteFileAtomic(S().cfg.dataDir + "mp_company_cfg.txt", content, &err)) {
        Log("[lobby] company cfg: %s\n", err.c_str());
        return;
    }
    Log("[lobby] company cfg -> %smp_company_cfg.txt: mode=%s me=%d ids=%s map=%s\n",
        S().cfg.dataDir.c_str(), mode.c_str(), mine, l3.c_str(), l4.c_str());
}

static void WritePlayerNames()
{
    std::string content;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        const Model& m = S().m;
        for (const auto& name : m.players)
            content += OriginLetterFor(m, name) + "=" + OneLine(name) + "\n";
    }
    std::string err;
    if (!WriteFileAtomic(S().cfg.dataDir + "mp_players.txt", content, &err))
        Log("[lobby] player names: %s\n", err.c_str());
}

// ---- commands to the lobby ----------------------------------------------------------------------
// One write() with O_APPEND: whole lines, even with the game script appending
// its own chat to the same file.
static void AppendIn(const std::string& line)
{
    if (S().lobbyDir.empty()) return;
    const std::string path = S().lobbyDir + "lobby_in.jsonl";
    const int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { Log("[lobby] cannot append to %s: %s\n", path.c_str(), strerror(errno)); return; }
    const std::string l = line + "\n";
    const ssize_t w = write(fd, l.data(), l.size());
    if (w != (ssize_t)l.size()) Log("[lobby] short write to %s\n", path.c_str());
    close(fd);
}

static void Queue(Request r)
{
    {
        std::lock_guard<std::mutex> lk(S().qMtx);
        S().q.push_back(std::move(r));
    }
    S().qCv.notify_one();
}

static void QueueLine(uint64_t gen, const std::string& line)
{
    Request r{ Request::Line, gen, StartRequest(), line };
    Queue(std::move(r));
}

// Lobby thread: the chat line lobby.py waits for (menu_hook.cpp SendChat).
static void SendChatNow(const std::string& text)
{
    bool ready;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        ready = S().m.lobbyReady;
    }
    if (!ready) { Status("Lobby is starting\xE2\x80\xA6"); return; }
    AppendIn("{\"cmd\":\"chat\",\"text\":\"" + JsonEscape(ValidUtf8(text)) + "\"}");
}

// ---- where the game is ----------------------------------------------------------------------------
// menu_hook.cpp: inGame = !g_showOverlay && g_gameUi; the title menu at the
// front clears the CGameUI capture.
static bool InGame() { return !g_titleMenu.load() && g_gameUiSeen.load(); }

// ---- HOT JOIN (menu_hook.cpp SyncStart / SyncPoll) --------------------------------------------------
// Owned by the lobby thread. Unknown pause state ages the snapshot normally.
static std::string OwnLetter()
{
    std::lock_guard<std::mutex> lk(S().mtx);
    return S().m.you.empty() ? "a" : OriginLetterFor(S().m, S().m.you);
}
static void UnpausedTick()
{
    const uint64_t now = NowMs();
    std::string dash;
    ReadSmallFile(S().cfg.dataDir + "lockstep_dash_" + OwnLetter() + ".txt", &dash);
    if (S().unpausedLast && dash.find("paused=yes") == std::string::npos)
        S().unpausedMs += now - S().unpausedLast;
    S().unpausedLast = now;
}
static void MarkSaveShared(const std::string& path)
{
    S().sharedSave = path;
    S().sharedUnpaused = S().unpausedMs;
}
static void ReportStage(const std::string& text)
{
    if (text == S().stageSent) return;
    AppendIn("{\"cmd\":\"stage\",\"text\":\"" + JsonEscape(text) + "\"}");
    S().stageSent = text;
}
static void StageTick()
{
    if (!S().stageWatch || !InGame()) return;
    std::string status;
    ReadSmallFile(S().cfg.dataDir + "lockstep_status_" + OwnLetter() + ".txt", &status);
    auto start = status.find("stage=");
    if (start == std::string::npos) { ReportStage("world loaded"); return; }
    start += 6;
    std::string stage = status.substr(start, status.find_first_of(" \r\n\t", start) - start);
    if (stage == "live") { ReportStage(""); S().stageWatch = false; }
    else if (stage == "starting") ReportStage("world loaded, waiting for the session");
    else if (stage.compare(0, 8, "catchup:") == 0) {
        auto colon = stage.find(':', 8);
        double behind = colon == std::string::npos ? 0 : std::atof(stage.c_str() + colon + 1);
        char text[128];
        std::snprintf(text, sizeof(text), stage.compare(8, 5, "fetch") == 0 ?
            "catching up: fetching history (%.0f s behind)" : "catching up (%.0f s behind)", behind);
        ReportStage(text);
    } else if (stage.compare(0, 7, "behind:") == 0) {
        char text[80]; std::snprintf(text, sizeof(text), "%.0f s behind", std::atof(stage.c_str()+7)); ReportStage(text);
    }
}

static void SyncStart(const std::string& why)
{
    bool isHost;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        isHost = S().m.isHost;
    }
    if (!isHost) { Log("[sync] %s on a joiner -- ignored (the host saves)\n", why.c_str()); return; }
    if (!g_gameUiSeen.load()) { Log("[sync] %s before the game is running -- ignored\n", why.c_str()); return; }
    if (S().syncAskedAt) { Log("[sync] %s while a save is pending -- one save serves everyone who joined\n", why.c_str()); return; }
    UnpausedTick();
    if (why.compare(0, 8, "hot join") == 0 && !S().sharedSave.empty() &&
        Exists(S().sharedSave) && S().unpausedMs - S().sharedUnpaused < 15000) {
        AppendIn("{\"cmd\":\"start\",\"save\":\"" + JsonEscape(S().sharedSave) + "\"}");
        Status("Hot join: sending the recent save");
        SendChatNow("!hotjoin A game is running. Hold on: the host is sending you the world; your game loads it by itself.");
        return; // explicit start also latches a session that previously had no joiners
    }
    std::string cur;
    uint64_t size = 0;
    S().syncBaseline = MenuGame_NewestSave(&cur) ? MtimeNs(cur, &size) : 0;
    S().syncLastSize = 0;
    S().syncSave.clear();
    Log("[sync] %s -> taking the save\n", why.c_str());
    if (!MenuGame_ForceAutosave()) { S().switchShare = false; Log("[sync] the forced autosave did not start\n"); return; }
    AppendIn("{\"cmd\":\"sync_taking\"}");
    S().syncAskedAt = NowMs();
    Status("Hot join: saving\xE2\x80\xA6");
    // A marked chat line for the newcomer's panel -- not for the relay's
    // periodic upload, which put it in every chat every two minutes.
    if (why.compare(0, 12, "world switch") == 0)
        SendChatNow("!hotjoin The host has moved to another world. Hold on: it is being sent to you.");
    else if (why.compare(0, 6, "relay:") != 0)
        SendChatNow("!hotjoin A game is running. Hold on: the host is saving and will send you the world; your game loads it by itself.");
}

// The Lua token covers NEW GAME as well as loads. No Windows object layout.
static void PollWorldGen()
{
    std::string text;
    if (!ReadSmallFile(S().cfg.dataDir + "tpf2mp_world_gen.txt", &text)) return;
    std::string gen, pid;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find_first_of("\r\n", at);
        const std::string line = text.substr(at, end - at);
        if (line.compare(0, 4, "gen=") == 0) gen = line.substr(4);
        if (line.compare(0, 4, "pid=") == 0) pid = line.substr(4);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (gen.empty() || (!pid.empty() && pid != std::to_string(getpid())) || gen == S().worldGen) return;
    const bool first = S().worldGen.empty(), mine = S().worldGenHold;
    S().worldGen = gen; S().worldGenHold = false;
    if (first || mine) return;
    // A cached snapshot of the previous world is never reusable.
    S().sharedSave.clear();
    bool live;
    { std::lock_guard<std::mutex> lk(S().mtx);
      live = S().m.isHost && S().m.lobbyReady && S().m.players.size() >= 2; }
    if (!live || !S().sessionStarted || !InGame()) return;
    // Invalidate any older world's pending file watcher before taking this save.
    S().syncAskedAt = 0;
    S().switchShare = true;
    SyncStart("world switch");
}

static void SyncPoll()
{
    UnpausedTick();
    StageTick();
    PollWorldGen();
    const std::string req = S().cfg.dataDir + "tpf2_sync_save.txt";
    if (Exists(req)) {
        unlink(req.c_str());
        SyncStart("sync request (chat or button)");
    }
    if (!S().syncAskedAt) return;
    std::string cur;
    uint64_t sz = 0;
    if (MenuGame_NewestSave(&cur)) {
        const uint64_t mt = MtimeNs(cur, &sz);
        if (mt > S().syncBaseline && sz > 0) {
            // wait until the file stops growing (the sidecars are written after the .sav)
            if (cur == S().syncSave && sz == S().syncLastSize) {
                AppendIn("{\"cmd\":\"start\",\"save\":\"" + JsonEscape(cur) + "\"" +
                         (S().switchShare ? ",\"switch\":true" : "") + "}");
                S().switchShare = false;
                MarkSaveShared(cur);
                { std::lock_guard<std::mutex> lk(S().mtx); S().m.startSave = cur; }
                Log("[sync] new save %s (%llu B) -> sharing with every joiner\n", cur.c_str(), (unsigned long long)sz);
                Status("Sync: sharing the save\xE2\x80\xA6");
                std::string err;
                if (!WriteFileAtomic(S().cfg.dataDir + "tpf2_sync_sent.txt", cur, &err)) Log("[sync] tpf2_sync_sent.txt: %s\n", err.c_str());
                S().syncAskedAt = 0;
                return;
            }
            S().syncSave = cur;
            S().syncLastSize = sz;
        }
    }
    if (NowMs() - S().syncAskedAt > 90000) {
        Log("[sync] no new save appeared within 90 s -- giving up (is the save folder writable? see the game log)\n");
        Status("Sync: the save did not appear");
        S().switchShare = false;
        S().syncAskedAt = 0;
    }
}

// A relay lobby's copy of the world is whatever was uploaded last, so the
// leader refreshes it on a timer.
static void RelayPeriodic()
{
    const int minutes = S().cfg.relayAutosaveMin;
    if (minutes <= 0) return;
    bool relay, isHost;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        relay = S().m.relay;
        isHost = S().m.isHost;
    }
    if (!relay || !isHost || !InGame()) return;
    const uint64_t now = NowMs();
    if (!S().relayLastUp) S().relayLastUp = now;
    if (now - S().relayLastUp >= (uint64_t)minutes * 60000ull) {
        S().relayLastUp = now;
        SyncStart("relay: periodic save");
    }
}

// ---- events -------------------------------------------------------------------------------------------
static void ApplyRoster(const Json& ev)
{
    bool roleKnown, isHost, relay, relayRoleChanged = false;
    int count, prevCount;
    long age, mx;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        Model& m = S().m;
        m.players.clear();
        if (const Json* pa = ev.Get("players"))
            if (pa->type == Json::Arr)
                for (const Json& it : pa->items) {
                    m.players.push_back(it.type == Json::Str ? it.s : std::string());
                }
        m.stages.assign(m.players.size(), std::string());
        if (const Json* stages = ev.Get("stages"))
            for (size_t i = 0; i < m.players.size(); ++i) m.stages[i] = JStr(*stages, m.players[i].c_str());
        if (ev.Get("mode")) m.separateCompanies = JStr(ev, "mode") == "companies";
        m.companies.assign(m.players.size(), 1);
        if (const Json* co = ev.Get("companies"))
            for (size_t i = 0; i < m.players.size(); i++) {
                const Json* v = co->Get(m.players[i].c_str());
                if (v && v->type == Json::Num && v->n >= 1 && v->n <= MAX_COMPANIES) m.companies[i] = (int)v->n;
            }
        std::string v = JStr(ev, "you");
        if (!v.empty()) m.you = v;
        v = JStr(ev, "host");
        if (!v.empty()) m.host = v;
        m.title = JStr(ev, "lobby");
        m.relay = JBool(ev, "relay", false);
        m.storedAge = JInt(ev, "stored_age", -1);
        m.storedMax = JInt(ev, "stored_max", -1);
        m.letters.assign(m.players.size(), std::string());
        if (const Json* lm = ev.Get("letters"))
            if (m.relay)
                for (size_t i = 0; i < m.players.size(); i++) {
                    const Json* l = lm->Get(m.players[i].c_str());
                    if (l && l->type == Json::Str && ValidLetter(l->s)) m.letters[i] = l->s;
                }
        // the role comes from the roster, re-evaluated on every one (a host
        // change re-points the bridge; the ctl write is a no-op when nothing changed)
        roleKnown = !m.you.empty() && !m.host.empty();
        isHost = roleKnown && m.you == m.host;
        relay = m.relay;
        if (roleKnown && relay) {
            relayRoleChanged = m.isHost != isHost;
            m.isHost = isHost;   // a relay lobby's leader takes the host's part
        }
        count = (int)m.players.size();
        age = m.storedAge;
        mx = m.storedMax;
        prevCount = m.lastCount;
        m.lastCount = count;
    }
    Dirty();
    if (relayRoleChanged) {
        Log("[lobby] relay lobby: we are %s the leader now (relay %s a saved world)\n",
            isHost ? "" : "not", age >= 0 ? "holds" : "has no");
        Status(age >= 0 ? "Loading the relay's world..."
               : isHost ? "This relay has no saved world yet -- press START GAME to send your most recent save."
                        : "Waiting for the leader to press START GAME.");
    }
    WritePlayerNames();
    if (roleKnown) WriteBridgeCtl(isHost);
    // HOT JOIN: the roster grew while we host a running game -- take a save and
    // share it; the newcomer's game loads it by itself.
    if (isHost && count > prevCount && prevCount > 0) {
        if (InGame()) {
            if (relay && age >= 0 && mx > 0 && age <= mx)
                Log("[lobby] hot join: roster %d -> %d -- the relay serves its %ld s old world, no sync taken\n", prevCount, count, age);
            else
                SyncStart("hot join: roster " + std::to_string(prevCount) + " -> " + std::to_string(count));
        } else if (!g_titleMenu.load() && !g_gameUiSeen.load() && !S().loggedNoGameUi) {
            S().loggedNoGameUi = true;
            Log("[lobby] hot join: roster %d -> %d away from the title menu, but no CGameUI frame has been reported "
                "(panel::OnGameUiFrame) -- no sync save is taken\n", prevCount, count);
        }
    }
}

static void ChatPush(const std::string& from, const std::string& text)
{
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        Model& m = S().m;
        m.chat.push_back(Cap(OneLine(from.empty() ? text : from + ": " + text), 400));
        while (m.chat.size() > CHAT_LINES) m.chat.pop_front();
    }
    Dirty();
}

// "/speed 2.5" and "/sync" typed in the chat by anyone: the host's game script
// acts on the speed= and sync= lines of the ctl (menu_hook.cpp speedFromChat).
static void SpeedFromChat(const std::string& text)
{
    bool isHost;
    if (text.compare(0, 5, "/sync") == 0) {
        size_t a = 5;
        while (a < text.size() && text[a] == ' ') a++;
        const bool off = text.compare(a, 3, "off") == 0;
        int now;
        {
            std::lock_guard<std::mutex> lk(S().mtx);
            S().m.syncReq = off ? 0 : S().m.syncReq + 1;
            now = S().m.syncReq;
            isHost = S().m.isHost;
        }
        Log("[lobby] chat /sync -> %d\n", now);
        WriteBridgeCtl(isHost);
        return;
    }
    if (text.compare(0, 6, "/speed") != 0) return;
    const char* p = text.c_str() + 6;
    const char* end = text.c_str() + text.size();
    while (p < end && *p == ' ') p++;
    double v = 0;
    if (!ParseNumber(p, end, &v)) v = 0;
    const std::string req = (v > 0.0 && v < 64.0) ? FormatSpeed(v) : std::string();
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        S().m.speedReq = req;
        isHost = S().m.isHost;
    }
    Log("[lobby] chat /speed -> %s\n", req.empty() ? "off" : req.c_str());
    WriteBridgeCtl(isHost);
}

// Place the shared save and start it (menu_hook.cpp doStartLoad).
static bool DoStartLoad(const std::string& src)
{
    if (src.empty()) { Log("[lobby] start: no save path\n"); Status("No save to load."); return false; }
    std::string placed;
    if (!MenuGame_PlaceSharedSave(src, &placed)) {
        Log("[lobby] placing %s as the shared save failed\n", src.c_str());
        Status("Couldn't place the shared save -- not loading");
        return false;
    }
    const std::string name = placed.empty() ? std::string("mp_shared") : placed;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        S().m.lobbyDone = true;   // release the keyboard: the lobby's work is done
    }
    g_captures = false;
    Dirty();
    if (S().cfg.autoload) {
        Log("[lobby] %s placed as %s -- asking for its in-process load\n", src.c_str(), name.c_str());
        Status("Save ready -- loading it...");   // first: a refusal from the autoload may replace it
        MenuGame_RequestAutoload(name);
        return true;
    }
    Log("[lobby] %s placed as %s -- autoload=0: the player loads it from LOAD GAME\n", src.c_str(), name.c_str());
    Status("Save ready -- open LOAD GAME and pick \"" + name + "\".");
    return true;
}

// UI thread: snapshot the session identity, then leave all file IO to the lobby.
static void OnMenuLoad(const char* name)
{
    uint64_t gen;
    { std::lock_guard<std::mutex> lk(S().mtx);
      if (!S().m.isHost || !S().m.lobbyReady || S().m.players.size()<2) return;
      gen=S().m.gen; }
    Queue(Request{Request::LoadedSave,gen,StartRequest(),name});
}
static void ShareLoadedSave(const std::string& name)
{
    const std::string path=MenuGame_SaveDir()+"/"+name+".sav";
    if (!Exists(path)) { Log("[menu] accepted save %s not found; not shared\n",path.c_str()); return; }
    const bool switching=InGame() || S().sessionStarted;
    S().worldGenHold=true; S().hostLoadedItself=true;
    S().syncAskedAt=0; S().switchShare=false;
    { std::lock_guard<std::mutex> lk(S().mtx); S().m.startSave=path; }
    WriteCompanyCfg();
    AppendIn("{\"cmd\":\"start\",\"save\":\""+JsonEscape(path)+"\""+(switching ? ",\"switch\":true" : "")+"}");
    MarkSaveShared(path);
    Status("Sharing the world loaded by the host");
}

static void HandleStart(const Json& ev)
{
    // save=true: a save transfer completed for this peer this session; absent = true
    const bool withSave = JBool(ev, "save", true);
    bool saveReady, isHost;
    std::string startSave;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        saveReady = S().m.saveReady;
        isHost = S().m.isHost;
        startSave = S().m.startSave;
    }
    if (isHost) {
        S().sessionStarted = true;
        if (S().hostLoadedItself) { S().hostLoadedItself=false; return; }
    }
    const bool switching = JBool(ev, "switch", false);
    if (switching) {
        if (isHost) return; // the host already loaded the new world
        if (!(withSave && saveReady)) {
            Status("The host switched world but its save did not arrive."); return;
        }
        { std::lock_guard<std::mutex> lk(S().mtx); S().m.saveReady = false; }
        S().worldGenHold = true;
        if (InGame()) {
            // Linux has no verified in-place load controller. Place the current
            // transfer and give the player an actionable manual load fallback.
            std::string placed;
            WriteCompanyCfg();
            if (MenuGame_PlaceSharedSave(S().lobbyDir + "incoming_save.sav", &placed))
                Status("The host changed world -- open LOAD GAME and pick \"mp_shared\".");
            else Status("Could not place the host's new save -- ask the host to START again.");
            return;
        }
    }
    std::string src;
    bool go = true;
    if (InGame()) {
        // HOT JOIN: we are playing; this start is the sync save for a newcomer
        Log("[lobby] start while in game -- a sync for a newcomer, ignored here\n");
        go = false;
    }
    if (isHost && !(withSave && saveReady)) {
        // our own save; a leader that RECEIVED a save this session loads that instead
        src = startSave;
        if (src.empty()) {
            Log("[lobby] start: we shared no save this session -- not loading\n");
            Status("No save was shared -- press START GAME again");
            go = false;
        }
    } else if (saveReady) {
        src = S().lobbyDir + "incoming_save.sav";
    } else {
        // loading our own newest save would put this player in another world
        Log("[lobby] start(save=%d) but no save_ready this session -- not loading\n", withSave ? 1 : 0);
        Status("Start received but no save arrived -- ask the host to START again");
        go = false;
    }
    if (!go) return;
    WriteCompanyCfg();
    Status("Loading shared save\xE2\x80\xA6");
    SleepMs(400);
    // The lobby stays: since the game-frame relay it IS the lockstep transport.
    S().worldGenHold = true;
    if (DoStartLoad(src)) {
        ReportStage("loading world"); S().stageWatch = true;
        Log("[lobby] game loading -- lobby kept alive as the game transport\n");
    }
}

static void Dispatch(const std::string& line)
{
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        if (S().m.gen != S().child.gen) return;   // left, or another lobby is on its way
        S().m.lobbyReady = true;
    }
    Json ev;
    if (!ParseJson(line, &ev) || ev.type != Json::Obj) {
        static int bad = 0;
        if (bad++ < 10) Log("[lobby] unreadable event line (%zu B): %.120s\n", line.size(), line.c_str());
        return;
    }
    const std::string ty = JStr(ev, "type");
    if (ty == "code") {
        const std::string cd = JStr(ev, "code");
        if (cd.empty()) return;
        // The copy waits for the next input or window event on the UI thread
        // (lobby_linux.h CopyCode), and a host who tabbed away sends none: say
        // so first. Before the code is stored, so the copy's "Your code is
        // copied" always comes after this line and replaces it.
        Status("Room code ready -- move the mouse over the game to copy it.");
        {
            std::lock_guard<std::mutex> lk(S().mtx);
            S().m.code = cd;
            S().m.haveCode = true;
        }
        g_autoCopy = true;   // the panel copies it on the UI thread, then says so
        Log("[lobby] room code received (%zu chars, not logged)\n", cd.size());
        Dirty();
    } else if (ty == "roster") {
        ApplyRoster(ev);
    } else if (ty == "chat") {
        const std::string from = JStr(ev, "from"), text = JStr(ev, "text");
        if (text.compare(0, 9, "!hotjoin ") == 0) {
            // the host is saving for a newcomer: only a panel at the title menu shows it
            if (g_titleMenu.load() && !g_gameUiSeen.load()) Status(Cap(OneLine(text.substr(9)), 300));
        } else {
            ChatPush(from, text);
            SpeedFromChat(text);
        }
    } else if (ty == "status") {
        const std::string detail = JStr(ev, "detail");
        if (!detail.empty()) Status(Cap(OneLine(detail), 300));
    } else if (ty == "transfer") {
        const std::string role = JStr(ev, "role"), st = JStr(ev, "state"), peer = JStr(ev, "peer");
        const long pct = JInt(ev, "pct", -1);
        const bool toRelay = peer == "relay";
        std::string msg, xfer;
        bool setX = false;
        const std::string p = std::to_string(pct) + "%";
        if (role == "recv") {
            if (pct >= 0) { msg = "Receiving save\xE2\x80\xA6 " + p; xfer = "receiving " + p; setX = true; }
        } else if (st == "done") {
            msg = toRelay ? "Save uploaded to the relay." : "Save sent.";
            setX = true;
        } else if (!st.empty()) {
            setX = true;
        } else if (pct >= 0) {
            msg = (toRelay ? "Uploading save to the relay\xE2\x80\xA6 " : "Sending save\xE2\x80\xA6 ") + p;
            xfer = (toRelay ? "uploading " : "sending ") + p;
            setX = true;
        }
        if (pct >= 100) { xfer.clear(); setX = true; }
        bool isHost;
        {
            std::lock_guard<std::mutex> lk(S().mtx);
            if (setX) S().m.xfer = xfer;
            isHost = S().m.isHost;
        }
        if (!msg.empty()) Status(msg);
        WriteBridgeCtl(isHost);   // the in-game window reads xfer= from the ctl
    } else if (ty == "mods_prompt") {
        const long n = JInt(ev, "count", -1);
        const std::string text = Cap(OneLine(JStr(ev, "text")), 200);
        if (S().cfg.shareMods == 1) {
            AppendIn("{\"cmd\":\"mods\",\"accept\":true}");
            Status("Downloading the mods this save needs from the host\xE2\x80\xA6");
        } else if (S().cfg.shareMods == 2) {
            AppendIn("{\"cmd\":\"mods\",\"accept\":false}");
            Status("Mod download is off (share_mods=never).");
        } else {
            {
                std::lock_guard<std::mutex> lk(S().mtx);
                S().m.modsPrompt = "Download " + std::to_string(n) + " mod(s) this save needs from the host? (" + text + ")";
            }
            Dirty();
        }
    } else if (ty == "mods_ready") {
        Status("Mods received \xE2\x80\x94 waiting for start\xE2\x80\xA6");
    } else if (ty == "save_ready") {
        {
            std::lock_guard<std::mutex> lk(S().mtx);
            S().m.saveReady = true;
        }
        Status("Save received \xE2\x80\x94 waiting for start\xE2\x80\xA6");
    } else if (ty == "start") {
        HandleStart(ev);
    }
}

// ---- the lobby thread ------------------------------------------------------------------------------------
static void TailOut()
{
    Child& c = S().child;
    const std::string path = S().lobbyDir + "lobby_out.jsonl";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) == 0 && (uint64_t)st.st_size < c.offset) {
        Log("[lobby] lobby_out.jsonl shrank (%lld < %llu) -- reading it from the start\n", (long long)st.st_size, (unsigned long long)c.offset);
        c.offset = 0;
        c.partial.clear();
    }
    char buf[8192];
    for (;;) {
        const ssize_t got = pread(fd, buf, sizeof(buf), (off_t)c.offset);
        if (got <= 0) break;
        c.offset += (uint64_t)got;
        for (ssize_t i = 0; i < got; i++) {
            if (buf[i] != '\n') {
                c.partial.push_back(buf[i]);
                continue;
            }
            std::string line;
            line.swap(c.partial);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            c.lines++;
            if (!line.empty()) Dispatch(line);
        }
    }
    close(fd);
}

static void ForgetChild()
{
    g_childPid = 0;
    S().child = Child();
    S().worldGen.clear(); S().sessionStarted = S().worldGenHold = S().switchShare = S().hostLoadedItself = false;
    S().sharedSave.clear(); S().unpausedLast = 0; S().stageWatch = false; S().stageSent.clear();
    S().syncAskedAt = 0;
}

// Quit, then SIGTERM, then SIGKILL, always to the whole group (menu_hook.cpp
// QuitLobbyProc; the group is what TerminateJobObject took).
static void Teardown()
{
    const pid_t pid = S().child.pid;
    if (!pid) return;
    AppendIn("{\"cmd\":\"quit\"}");
    Exit e = Exit::Running;
    int st = 0;
    const char* how = "on quit";
    if (WaitExit(pid, QUIT_WAIT_MS, &e)) st = SweepAndReap(pid, e);
    else how = StopGroup(pid, TERM_WAIT_MS, &st);
    if (how)
        Log("[lobby] lobby exited %s (%s)\n", how, e == Exit::Gone ? "reaped elsewhere" : DescribeStatus(st).c_str());
    else
        Log("[lobby] lobby did not exit within %d ms of quit and %d ms of SIGTERM -- killed its process group\n", QUIT_WAIT_MS, TERM_WAIT_MS);
    ForgetChild();
}

// The lobby ended by itself: say why (menu_hook.cpp, after the tail loop).
static void CheckExit()
{
    const pid_t pid = S().child.pid;
    const Exit e = HasExited(pid);
    if (e == Exit::Running) return;
    TailOut();   // its last words
    const int st = SweepAndReap(pid, e);
    const long lines = S().child.lines;
    bool current;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        current = S().m.gen == S().child.gen && S().m.active;
        if (current) S().m.dead = true;   // chat, START and PUBLIC refuse with kNotRunning from now on
    }
    Log("[lobby] lobby process exited (%s) after %ld event line(s)\n", e == Exit::Gone ? "reaped elsewhere" : DescribeStatus(st).c_str(), lines);
    if (current && lines == 0) Status("The lobby stopped before it reported anything -- see tpf2_menu.log");
    ForgetChild();
    if (current) Dirty();
}

// A launch failed: see kNotRunning. Not for a lobby already left or replaced.
static void MarkDead(uint64_t gen)
{
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        if (S().m.gen != gen) return;
        S().m.dead = true;
    }
    Dirty();
}

static bool StillCurrent(uint64_t gen)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    return S().m.gen == gen && S().m.active;
}

// The command for the log. The password and a joiner's room code are
// credentials: the log rides to the host's lobby_peers.log (--forward-log) and
// into OPEN LOGS archives.
static std::string Shown(const std::vector<std::string>& argv)
{
    std::string s;
    for (size_t i = 0; i < argv.size(); i++) {
        const std::string& a = argv[i];
        if (i) s += ' ';
        if (a.compare(0, 11, "--password=") == 0) { s += "--password=***"; continue; }
        if (i > 0 && argv[i - 1] == "join") { s += "<code, " + std::to_string(a.size()) + " chars>"; continue; }
        if (a.find_first_of(" '\"") != std::string::npos) s += "\"" + a + "\"";
        else s += a;
    }
    return s;
}

// The environment the lobby program is started with (NETPUNCH.md 3.2).
static std::vector<std::string> LobbyEnv()
{
    std::vector<std::string> env;
    if (!S().cfg.gameDir.empty()) env.push_back("TPF2MP_GAME_DIR=" + NoSlash(S().cfg.gameDir));
    return env;
}

static void Launch(const Request& r)
{
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        if (S().m.gen != r.gen) return;   // LEAVE, or another HOST / JOIN, came first
    }
    std::string readiness;
    if (!CancellationReady(&readiness)) {
        MarkDead(r.gen);
        Status(readiness);
        return;
    }
    const StartRequest& a = r.start;
    Program prog;
    std::string tried;
    if (!ResolveProgram(&prog, &tried)) {
        Log("[lobby] no lobby program found (tried %s) -- the lobby cannot start\n", tried.c_str());
        MarkDead(r.gen);   // first: a chat sent in between must not say "starting" over the reason
        Status("The lobby program is not installed (netpunch) -- see tpf2_menu.log");
        return;
    }
    const std::string dir = LobbyFolder(prog);
    S().lobbyDir = dir;
    const int relayPort = PickRelayPort(a.join);
    if (a.join && relayPort != GAME_RELAY_PORT_JOIN)
        Log("[lobby] relay port %d is taken (another joiner on this machine) -> using %d\n", GAME_RELAY_PORT_JOIN, relayPort);
    const int bridgePort = ReadBridgePort();
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        S().m.relayPort = relayPort;
    }

    // menu_hook.cpp's arguments, plus --io-dir and --parent-pid (NETPUNCH.md
    // 3.3). Values a player typed use --opt=value: argparse would take a
    // leading '-' for another option.
    std::vector<std::string> argv = prog.argv0;
    const std::string rp = std::to_string(relayPort), bp = std::to_string(bridgePort);
    if (a.join) {
        for (const std::string& s : { std::string("join"), a.code, "--name=" + a.name, std::string("--local-port"), std::string("0"),
                                      std::string("--game-relay-port"), rp, std::string("--game-local-port"), bp })
            argv.push_back(s);
    } else {
        for (const std::string& s : { std::string("host"), "--name=" + a.name, std::string("--game-relay-port"), rp,
                                      std::string("--game-local-port"), bp })
            argv.push_back(s);
    }
    // every per-machine log rides to the host's merged lobby_peers.log
    for (const char* f : { "tpf2_bridge.log", "tpf2_menu.log", "mp_company_a.log", "mp_company_b.log" }) {
        argv.push_back("--forward-log");
        argv.push_back(S().cfg.dataDir + f);
    }
    if (!a.password.empty()) argv.push_back("--password=" + a.password);
    if (!a.join) {
        argv.push_back("--lobby-name=" + a.lobbyName);
        if (a.separateCompanies) argv.push_back("--companies");
        if (!S().cfg.masterUrl.empty()) {
            argv.push_back("--publish=" + S().cfg.masterUrl);
            if (a.pub) argv.push_back("--public");
        }
        if (S().cfg.shareMods == 2) argv.push_back("--no-share-mods");
    }
    argv.push_back("--io-dir=" + NoSlash(dir));
    argv.push_back("--parent-pid");
    argv.push_back(std::to_string((long)getpid()));
    Log("[lobby] lobby cmd (in %s): %s\n", dir.c_str(), Shown(argv).c_str());

    for (const char* f : { "lobby_out.jsonl", "lobby_in.jsonl" }) unlink((dir + f).c_str());
    if (a.join)   // never let the last session's transfer pass for this one (lobby.py does this too)
        for (const char* f : { "incoming_save.sav", "incoming_save.sav.lua", "incoming_save.jpg" }) unlink((dir + f).c_str());

    const std::string logPath = dir + "lobby_proc.log";
    const bool keepLogs = access((S().cfg.dataDir + "tpf2mp_keep_logs.txt").c_str(), F_OK) == 0;
    const int logFd = HighFd(open(logPath.c_str(), O_WRONLY | O_CREAT | (keepLogs ? O_APPEND : O_TRUNC) | O_CLOEXEC, 0644));
    if (keepLogs && logFd >= 0) dprintf(logFd, "\n==== lobby session %ld pid %ld (keeping logs) ====\n", (long)time(nullptr), (long)getpid());
    if (logFd < 0) Log("[lobby] cannot open %s (%s) -- the lobby runs without its output captured\n", logPath.c_str(), strerror(errno));
    Status(a.join ? "Joining lobby\xE2\x80\xA6" : "Starting lobby\xE2\x80\xA6");

    SpawnSpec sp;
    sp.argv = argv;
    sp.env = LobbyEnv();
    sp.cwd = dir;
    sp.stdoutFd = logFd;
    sp.stderrFd = logFd;
    sp.newSession = true;
    sp.deathSignal = SIGTERM;
    int err = 0;
    const pid_t pid = Spawn(sp, &err);
    if (logFd >= 0) close(logFd);
    if (pid <= 0) {
        Log("[lobby] starting %s failed: %s\n", argv[0].c_str(), strerror(err));
        MarkDead(r.gen);
        Status("Couldn't start the lobby -- see tpf2_menu.log");
        return;
    }
    snprintf(g_quitPath, sizeof(g_quitPath), "%slobby_in.jsonl", dir.c_str());
    Child c;
    c.pid = pid;
    c.gen = r.gen;
    S().child = c;
    g_childPid = pid;
    S().ctlLast.clear();
    Log("[lobby] lobby running: pid %d, its own session, SIGTERM if the game dies\n", (int)pid);
}

// Lobby thread: this game's newest save goes to every joiner, then start
// (menu_hook.cpp OnHit case 6).
static void ShareAndStart()
{
    std::string readiness;
    if (!CancellationReady(&readiness)) { Status(readiness); return; }
    std::string save;
    if (MenuGame_NewestSave(&save) && !save.empty()) {
        {
            std::lock_guard<std::mutex> lk(S().mtx);
            S().m.startSave = save;
        }
        AppendIn("{\"cmd\":\"start\",\"save\":\"" + JsonEscape(save) + "\"}");
        MarkSaveShared(save);
        Log("[lobby] START GAME: sharing %s\n", save.c_str());
        Status("Sharing save & starting game\xE2\x80\xA6");
    } else {
        AppendIn("{\"cmd\":\"start\"}");
        Log("[lobby] START GAME: no save found in %s\n", MenuGame_SaveDir().c_str());
        Status("No save found to share.");
    }
}

static void LobbyThread()
{
    for (;;) {
        std::deque<Request> work;
        {
            std::unique_lock<std::mutex> lk(S().qMtx);
            S().qCv.wait_for(lk, std::chrono::milliseconds(200), [] { return !S().q.empty(); });
            work.swap(S().q);
        }
        for (const Request& r : work) {
            switch (r.kind) {
                case Request::Launch:
                    Teardown();   // a previous lobby would fight the new one over lobby_*.jsonl
                    Launch(r);
                    break;
                case Request::Stop:
                    Teardown();
                    break;
                case Request::Line:
                case Request::ShareStart:
                case Request::LoadedSave:
                    if (S().child.pid && S().child.gen == r.gen) {
                        if (r.kind == Request::Line) AppendIn(r.line);
                        else if (r.kind == Request::LoadedSave) ShareLoadedSave(r.line);
                        else ShareAndStart();
                    } else if (StillCurrent(r.gen)) {
                        // queued before CheckExit saw the lobby go (it looks every 200 ms)
                        Log("[lobby] %s dropped: the lobby is no longer running\n", r.kind == Request::Line ? "a command" : "START GAME");
                        Status(kNotRunning);
                    }
                    break;
            }
        }
        if (!S().child.pid) continue;
        TailOut();
        CheckExit();
        if (!S().child.pid) continue;
        SyncPoll();
        RelayPeriodic();
    }
}

// ---- the public game list (menu_hook.cpp PubFetchThread) ---------------------------------------------
// The lobby program fetches <master_url>/list (NETPUNCH.md 3.6): exit 0 with
// the body on stdout, or exit 1 with one line saying why ("HTTP 503"). No HTTP
// or TLS in the game process.
static bool FetchPublic(std::vector<PubRow>* rows, std::string* note)
{
    Program prog;
    std::string tried;
    if (!ResolveProgram(&prog, &tried)) { *note = "Server browser unavailable (the lobby program is not installed)"; return false; }
    int outP[2], errP[2];
    if (pipe2(outP, O_CLOEXEC) != 0) { *note = "Server browser unavailable (no pipe)"; return false; }
    if (pipe2(errP, O_CLOEXEC) != 0) { close(outP[0]); close(outP[1]); *note = "Server browser unavailable (no pipe)"; return false; }
    outP[1] = HighFd(outP[1]);
    errP[1] = HighFd(errP[1]);
    SpawnSpec sp;
    sp.argv = prog.argv0;
    sp.argv.push_back("--print-public-list");
    sp.argv.push_back(S().cfg.masterUrl);
    sp.env = LobbyEnv();
    sp.cwd = LobbyFolder(prog);
    sp.stdoutFd = outP[1];
    sp.stderrFd = errP[1];
    sp.newSession = true;
    sp.deathSignal = SIGTERM;
    int err = 0;
    const pid_t pid = Spawn(sp, &err);
    close(outP[1]);
    close(errP[1]);
    if (pid <= 0) {
        close(outP[0]);
        close(errP[0]);
        Log("[lobby] server browser: starting %s failed: %s\n", sp.argv[0].c_str(), strerror(err));
        *note = "Server browser unavailable (the lobby program did not start)";
        return false;
    }
    std::string body, errText;
    bool timedOut = false;
    pollfd fds[2] = { { outP[0], POLLIN, 0 }, { errP[0], POLLIN, 0 } };
    int open = 2;
    const uint64_t deadline = NowMs() + PUB_TIMEOUT_MS;
    while (open > 0) {
        const uint64_t now = NowMs();
        if (now >= deadline) { timedOut = true; break; }
        const int r = poll(fds, 2, (int)(deadline - now));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        for (int k = 0; k < 2; k++) {
            if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[4096];
            const ssize_t n = read(fds[k].fd, buf, sizeof(buf));
            if (n > 0) {
                std::string& dst = k == 0 ? body : errText;
                if (dst.size() < (k == 0 ? (512u << 10) : 8192u)) dst.append(buf, (size_t)n);
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                close(fds[k].fd);
                fds[k].fd = -1;
                open--;
            }
        }
    }
    for (pollfd& f : fds) if (f.fd >= 0) close(f.fd);
    int st = 0;
    Exit e = Exit::Running;
    if (timedOut || !WaitExit(pid, 3000, &e)) {
        StopGroup(pid, 1000, &st);
        timedOut = true;
    } else {
        st = SweepAndReap(pid, e);
    }
    const bool ok = !timedOut && (e == Exit::Gone || (WIFEXITED(st) && WEXITSTATUS(st) == 0));
    if (!ok) {
        // the program's one line on stdout says why; its stderr, or the exit, otherwise
        auto firstLine = [](const std::string& s) {
            const size_t b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return std::string();
            return s.substr(b, s.find_first_of("\r\n", b) - b);
        };
        auto lastLine = [](std::string s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            const size_t nl = s.find_last_of('\n');
            return nl == std::string::npos ? s : s.substr(nl + 1);
        };
        std::string why = timedOut ? std::string("no answer in 20 s") : firstLine(body);
        if (why.empty()) why = lastLine(errText);
        if (why.empty()) why = DescribeStatus(st);
        *note = "Server browser unavailable (" + Cap(OneLine(why), 80) + ")";
        return false;
    }
    Json doc;
    if (!ParseJson(body, &doc) || doc.type != Json::Obj) { *note = "Server browser unavailable (unreadable list)"; return false; }
    if (const Json* sv = doc.Get("servers"))
        for (const Json& o : sv->items) {
            if ((int)rows->size() >= PUB_ROWS) break;
            if (o.type != Json::Obj) continue;
            PubRow row;
            row.code = JStr(o, "code");
            if (row.code.empty() || row.code.size() > 255) continue;
            row.name = Cap(OneLine(JStr(o, "name")), 127);
            row.game = Cap(OneLine(JStr(o, "game")), 63);
            row.type = Cap(OneLine(JStr(o, "type")), 15);
            row.version = Cap(OneLine(JStr(o, "version")), 23);
            row.players = (int)JInt(o, "players", 0);
            row.max = (int)JInt(o, "max", 8);
            row.age = (int)JInt(o, "age", 0);
            row.locked = JInt(o, "locked", 0) != 0;
            rows->push_back(row);
        }
    if (rows->empty()) *note = "No public games right now.";
    return true;
}

static void PubThread()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(S().pubMtx);
            S().pubCv.wait(lk, [] { return S().pubWanted; });
            S().pubWanted = false;
        }
        std::vector<PubRow> rows;
        std::string note;
        const bool ok = FetchPublic(&rows, &note);
        int logged;
        {
            std::lock_guard<std::mutex> lk(S().pubMtx);
            S().pub = rows;
            S().pubNote = note;
            S().pubLast = NowMs();
            S().pubBusy = false;
            logged = S().pubLogged++;
        }
        if (logged % 30 == 0 || !ok) Log("[lobby] server browser: %zu game(s) %s\n", rows.size(), note.c_str());
        Dirty();
    }
}

// ---- the interface ------------------------------------------------------------------------------------------
void Init(const Config& cfg, Tpf2mpLogFn log, StatusFn status, DirtyFn dirty)
{
    static std::once_flag once;
    std::call_once(once, [&] {
        S().cfg = cfg;
        S().log = log;
        S().status = status;
        S().dirty = dirty;
        MenuGame_ObserveLoads(&OnMenuLoad);
        Program prog;
        std::string tried;
        const bool found = ResolveProgram(&prog, &tried);
        Log("[lobby] lobby program: %s; lobby folder %s; save folder %s; master_url=%s relay_autosave_min=%d share_mods=%s autoload=%d\n",
            found ? (prog.argv0.back() + " in " + prog.dir).c_str() : ("NOT FOUND (tried " + tried + ")").c_str(),
            XdgNetDir().empty() ? "(no XDG_DATA_HOME or HOME)" : XdgNetDir().c_str(),
            MenuGame_SaveDir().c_str(), cfg.masterUrl.empty() ? "(none: no public list)" : cfg.masterUrl.c_str(),
            cfg.relayAutosaveMin, cfg.shareMods == 1 ? "always" : cfg.shareMods == 2 ? "never" : "ask", cfg.autoload ? 1 : 0);
        // See THREADS (lobby_linux.h): this runs on menu_linux.cpp's Init thread.
        try {
            std::thread(LobbyThread).detach();
            g_inited = true;
        } catch (...) {
            g_initFailed = true;
            Log("[lobby] lobby thread not started (the system refused a thread) -- HOST and JOIN are refused\n");
        }
        if (!cfg.masterUrl.empty()) {
            try {
                std::thread(PubThread).detach();
                g_pubStarted = true;
            } catch (...) {
                Log("[lobby] public list thread not started (the system refused a thread) -- no server browser\n");
                std::lock_guard<std::mutex> lk(S().pubMtx);
                S().pubNote = "Server browser unavailable (no thread)";
            }
        }
    });
}

static bool Base32Code(const std::string& s)
{
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7') || c == '=')) return false;
    return true;
}

bool Start(const StartRequest& in, std::string* why)
{
    StartRequest r = in;
    if (r.join) {
        const size_t b = r.code.find_first_not_of(" \t\r\n");
        const size_t e = r.code.find_last_not_of(" \t\r\n");
        r.code = b == std::string::npos ? std::string() : r.code.substr(b, e - b + 1);
        if (r.code.size() < 8) { *why = "Paste or type your host's code in the field first."; return false; }
        // it becomes an argument: a crafted "code" must not smuggle options in
        if (r.code.size() > 200 || !Base32Code(r.code)) { *why = "That is not a valid code (letters A-Z and digits 2-7 only)."; return false; }
    }
    if (r.name.empty()) { *why = "Type a player name first."; return false; }
    if (!g_inited.load()) {
        *why = g_initFailed.load() ? "The lobby could not start its thread -- see tpf2_menu.log" : "The lobby is not ready yet.";
        return false;
    }
    if (!CancellationReady(why)) return false;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        Model fresh;
        fresh.gen = S().m.gen + 1;
        fresh.active = true;
        fresh.isHost = !r.join;
        fresh.separateCompanies = !r.join && r.separateCompanies;
        S().m = fresh;
        gen = fresh.gen;
    }
    g_autoCopy = false;
    g_captures = true;
    Queue(Request{ Request::Launch, gen, r, std::string() });
    return true;
}

void Leave()
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        S().m.gen++;
        S().m.active = false;
        gen = S().m.gen;
    }
    g_autoCopy = false;
    Queue(Request{ Request::Stop, gen, StartRequest(), std::string() });
}

std::string SendChat(const std::string& text)
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        if (!S().m.active) return std::string();
        if (S().m.dead) return kNotRunning;
        if (!S().m.lobbyReady) return "Lobby is starting\xE2\x80\xA6";
        gen = S().m.gen;
    }
    QueueLine(gen, "{\"cmd\":\"chat\",\"text\":\"" + JsonEscape(ValidUtf8(text)) + "\"}");
    return std::string();
}

std::string StartGame()
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        if (!S().m.active || !S().m.isHost) return std::string();
        if (S().m.dead) return kNotRunning;
        if (!S().m.lobbyReady) return "Lobby is starting\xE2\x80\xA6";
        gen = S().m.gen;
    }
    std::string readiness;
    if (!CancellationReady(&readiness)) return readiness;
    Queue(Request{ Request::ShareStart, gen, StartRequest(), std::string() });
    return std::string();
}

std::string SetSeparateCompanies(bool on)
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        if (!S().m.active || !S().m.isHost) return "Only the host can change the company mode.";
        if (S().m.dead) return kNotRunning;
        if (!S().m.lobbyReady) return "Lobby is starting...";
        gen = S().m.gen;
    }
    QueueLine(gen, on ? "{\"cmd\":\"mode\",\"mode\":\"companies\"}" : "{\"cmd\":\"mode\",\"mode\":\"coop\"}");
    return on ? "Separate companies: every player gets their own company." : "Co-op: everyone plays company 1 together.";
}

std::string SetPublic(bool on)
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        if (!S().m.active || !S().m.isHost)
            return on ? "Your game will be listed publicly when you host." : "Your game will not be listed.";
        if (S().m.dead) return kNotRunning;
        if (!S().m.lobbyReady) return "Lobby is starting\xE2\x80\xA6";
        gen = S().m.gen;
    }
    QueueLine(gen, on ? "{\"cmd\":\"publish\",\"on\":true}" : "{\"cmd\":\"publish\",\"on\":false}");
    return on ? "Listed in the public server browser." : "Removed from the public server browser.";
}

std::string AnswerMods(bool yes)
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        S().m.modsPrompt.clear();
        if (S().m.active && S().m.dead) return kNotRunning;
        gen = S().m.gen;
    }
    QueueLine(gen, yes ? "{\"cmd\":\"mods\",\"accept\":true}" : "{\"cmd\":\"mods\",\"accept\":false}");
    return yes ? "Downloading the mods this save needs from the host\xE2\x80\xA6" : "Mod download declined.";
}

// The next company id somebody already uses, then one brand-new id, then back to 1.
void CycleCompany(int i, bool previous)
{
    std::string name;
    int next = 0;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(S().mtx);
        const Model& m = S().m;
        if (!m.active || m.dead || !m.lobbyReady || i < 0 || i >= (int)m.players.size()) return;
        if (!m.isHost && m.players[(size_t)i] != m.you) return;
        name = m.players[(size_t)i];
        const int cur = m.companies[(size_t)i];
        std::vector<bool> used((size_t)MAX_COMPANIES + 2, false);
        int maxUsed = 0;
        for (int c2 : m.companies)
            if (c2 >= 1 && c2 <= MAX_COMPANIES) { used[(size_t)c2] = true; if (c2 > maxUsed) maxUsed = c2; }
        if (previous) {
            for (int c2 = cur - 1; c2 >= 1; c2--) if (used[(size_t)c2]) { next = c2; break; }
            if (!next) next = maxUsed;
            if (next == cur) return;
        } else {
            for (int c2 = cur + 1; c2 <= maxUsed; c2++) if (used[(size_t)c2]) { next = c2; break; }
            if (!next) next = (cur <= maxUsed && maxUsed < MAX_COMPANIES) ? maxUsed + 1 : 1;
        }
        if (next < 1) next = 1;
        gen = m.gen;
    }
    if (name.empty()) return;
    QueueLine(gen, "{\"cmd\":\"company\",\"player\":\"" + JsonEscape(name) + "\",\"id\":" + std::to_string(next) + "}");
}

bool CopyCode(std::string* code)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    if (!S().m.active || !S().m.haveCode) return false;
    *code = S().m.code;
    return true;
}

bool AutoCopyPending() { return g_autoCopy.load(std::memory_order_relaxed); }

bool TakeAutoCopy(std::string* code)
{
    if (!g_autoCopy.exchange(false)) return false;
    return CopyCode(code);
}

bool CapturesTyping() { return g_captures.load(std::memory_order_relaxed); }

void Snapshot(View* v)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    const Model& m = S().m;
    v->separateCompanies = m.separateCompanies;
    v->haveCode = m.haveCode;
    v->isHost = m.isHost;
    v->youAreHost = !m.you.empty() && m.you == m.host;
    v->lobbyDone = m.lobbyDone;
    v->title = OneLine(m.title);
    v->modsPrompt = m.modsPrompt;
    v->players.resize(m.players.size());
    for (size_t i = 0; i < m.players.size(); i++) {
        Player& p = v->players[i];
        p.name = OneLine(m.players[i]);
        p.stage = i < m.stages.size() ? OneLine(m.stages[i]) : std::string();
        p.company = m.companies[i] < 1 ? 1 : m.companies[i] > MAX_COMPANIES ? MAX_COMPANIES : m.companies[i];
        p.you = m.players[i] == m.you;
        p.host = m.players[i] == m.host;
    }
    v->chat.assign(m.chat.begin(), m.chat.end());
}

void PublicPoll()
{
    if (!g_pubStarted.load()) return;   // no master_url, or no thread (Init set the note)
    std::lock_guard<std::mutex> lk(S().pubMtx);
    const uint64_t now = NowMs();
    const bool due = S().pubLast == 0 || now - S().pubLast > PUB_EVERY_MS || S().pubForce;
    if (!due || S().pubBusy) return;
    S().pubForce = false;
    S().pubBusy = true;
    S().pubWanted = true;
    S().pubCv.notify_one();
}

void PublicRefresh()
{
    std::lock_guard<std::mutex> lk(S().pubMtx);
    S().pubForce = true;
}

void PublicSnapshot(std::vector<PubRow>* rows, std::string* note)
{
    std::lock_guard<std::mutex> lk(S().pubMtx);
    *rows = S().pub;
    *note = S().pubNote;
}

bool PublicRow(int index, PubRow* row)
{
    std::lock_guard<std::mutex> lk(S().pubMtx);
    if (index < 0 || index >= (int)S().pub.size()) return false;
    *row = S().pub[(size_t)index];
    return true;
}

// xdg-open, when the Steam runtime's container has one. Not tied to the game's
// life: the file manager may stay.
static bool OpenFolder(const std::string& dir)
{
    const std::string opener = FindInPath("xdg-open");
    if (opener.empty()) { Log("[lobby] OPEN LOGS: no xdg-open in PATH -- naming the folder only\n"); return false; }
    SpawnSpec sp;
    sp.argv = { opener, dir };
    sp.cwd = dir;
    int err = 0;
    const pid_t pid = Spawn(sp, &err);
    if (pid <= 0) { Log("[lobby] OPEN LOGS: starting %s failed: %s\n", opener.c_str(), strerror(err)); return false; }
    Exit e = Exit::Running;
    if (!WaitExit(pid, 5000, &e)) {
        Log("[lobby] OPEN LOGS: %s is still running after 5 s -- left to finish\n", opener.c_str());
        try {
            std::thread([pid] { Reap(pid); }).detach();
        } catch (...) {
            Log("[lobby] OPEN LOGS: no thread to reap %s (pid %d) -- it stays a zombie once it ends\n", opener.c_str(), (int)pid);
        }
        return true;
    }
    const int st = e == Exit::Gone ? 0 : Reap(pid);
    Log("[lobby] OPEN LOGS: %s %s -> %s\n", opener.c_str(), dir.c_str(), e == Exit::Gone ? "done" : DescribeStatus(st).c_str());
    return e == Exit::Gone || (WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

// OPEN LOGS (menu_hook.cpp CollectLogsThread): this run's logs beside the
// earlier runs the loader saved at start (logarchive_linux.h), then the folder
// is shown. Off the UI thread: copying a long game log takes a moment. Called
// from the SDL event filter with the panel's lock held: no Status() here, and
// no exception may leave (THREADS).
bool OpenLogs()
{
    if (g_logsBusy.exchange(true)) return true;   // already gathering: its status follows
    try {
        std::thread([] {
            Tpf2mpLogArchive a;
            const std::string game = S().cfg.gameDir;
            const bool ok = Tpf2mpArchiveLogsSafe(false, game.empty() ? nullptr : game.c_str(), &a) && a.folder[0];
            Log("[lobby] OPEN LOGS: %d file(s) copied, %d unreadable%s%s\n", a.files, a.skipped, ok ? " -> " : "", ok ? a.folder : "");
            const std::string show = ok ? std::string(a.folder) : a.root[0] ? std::string(a.root) : S().cfg.dataDir;
            const bool opened = OpenFolder(show);
            if (ok)
                Status("Logs gathered in " + std::string(a.root) + (opened ? " (opened)" : "") + " -- send the newest folders with a bug report.");
            else
                Status("No logs were found to gather" + (a.root[0] ? " (" + std::string(a.root) + ")" : std::string()) + ".");
            g_logsBusy = false;
        }).detach();
    } catch (...) {
        g_logsBusy = false;
        Log("[lobby] OPEN LOGS: thread not started (the system refused a thread) -- nothing gathered\n");
        return false;
    }
    return true;
}

void OnMenuPage(int page)
{
    if (page == 2) {
        g_titleMenu = true;
        // the title menu exists only when no game runs: forget the last session's
        // CGameUI, or a start at the title menu would look like one in a game
        g_gameUiSeen = false;
    } else if (page >= 3) {
        g_titleMenu = false;
    }
}

void OnGameUiFrame()
{
    if (!g_gameUiSeen.load(std::memory_order_relaxed)) g_gameUiSeen = true;
}

// At exit: ask the lobby to quit, never wait (menu_hook.cpp DLL_PROCESS_DETACH).
// PR_SET_PDEATHSIG and --parent-pid are what make sure it goes.
__attribute__((destructor))
static void LobbyUnload()
{
    if (!g_childPid.load()) return;
    const int fd = open(g_quitPath, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) return;
    static const char kQuit[] = "{\"cmd\":\"quit\"}\n";
    const ssize_t w = write(fd, kQuit, sizeof(kQuit) - 1);
    (void)w;
    close(fd);
}

}  // namespace lobby
