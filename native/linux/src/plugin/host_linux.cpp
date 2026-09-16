// tpf2_pluginhost.so -- the Linux build of plugin/host.cpp: loads native
// plugins and services them through the Tpf2mpHost table of tpf2mp_plugin.h
// (ABI 1, the same header the Windows host is built from).
//
// Why a host exists at all, and why it is deliberately not a mod manager, is
// commented in host.cpp. libtpf2mp_boot.so loads it last, as the proxy loads
// the DLL: a plugin must not be able to stop the multiplayer libraries coming up.
//
// WHAT CHANGES ON LINUX
//   - A plugin is plugins/*.so, opened with dlopen and entered through
//     dlsym("Tpf2mpPluginInit"). The export must be the library's own: dlsym
//     also searches its dependencies. None is ever dlclosed.
//   - Where: <data dir>plugins/, then <lib dir>plugins/ (the folder this library
//     was loaded from), then <game dir>plugins/. A file name found earlier hides
//     the same name later. On Windows the second place is the host DLL's folder,
//     which the installer makes the game folder. On Linux the libraries live in
//     $XDG_DATA_HOME/tpf2mp/ or ~/.local/share/tpf2mp/ (boot.cpp), apart from the
//     Steam-managed game folder, so both are searched, and a folder reached
//     twice is searched once.
//     tpf2mp.cfg the same way round: <lib dir>, <game dir>, then <data dir>;
//     the first file found wins. One that exists but cannot be read is skipped,
//     as on Windows, with a log line.
//   - Names are matched case-sensitively, and each folder is read in sorted
//     order: readdir gives hash order, which differs between machines.
//   - moduleBase is the executable's load address in a process whose image is
//     named TransportFever2 (boot.cpp's test), else 0. buildOk is the GNU
//     build-id (game_image.h) in that process.
//   - verifyBytes and patchBytes check the range against /proc/self/maps, where
//     host.cpp asked VirtualQuery: every byte mapped readable, and inside the
//     executable's PT_LOAD extent (an RVA past it is a wrong RVA). A maps file
//     that cannot be read is logged once, and every such call then refuses.
//   - patchBytes writes through /proc/self/mem (codewrite_linux.h), which
//     changes no page protection. Only when that file is not open, or the kernel
//     refuses the write, does it add PROT_WRITE to each mapping it touches and
//     put each back as it was. Every patch is logged, with the way it was written.
//   - installHook is hook_posix.cpp's InstallHook. The host refuses what it can
//     prove wrong: stolen bytes not mapped readable and executable, or a cut that
//     hook.h's prologue decoder shows lands inside an instruction. When the
//     decoder cannot say, the host installs on the plugin's word, with a log line:
//     bytes it does not know (RIP-relative ones included, which it reports the same
//     way), an instruction it is known to miscount (Imm16InstructionAt), or a
//     cut whose next bytes are not mapped for it to read.
//   - dataDir() ends in '/'.
//
// PAGE PROTECTION RACES
// The bridge, menu and slice libraries install their patches from init threads
// while plugins init here. hook_posix.cpp, near_alloc.h and setplayer_linux.cpp
// each do mprotect(rwx) / write / mprotect(r-x) with no lock between libraries.
// When two writes share a 4 KB page, one library can restore r-x between the
// other's mprotect and its write, and that write faults. patchBytes through
// /proc/self/mem takes no part in that. Its mprotect fallback and installHook
// still do, until hook_posix.cpp and the other patchers write through
// codewrite_linux.h too (docs/linux/PLUGIN_HOST.md, integration notes).
// g_patchMtx only keeps the host's own calls apart.
//
// WHEN PLUGINS RUN
// Synchronously, in this library's constructor. boot.cpp dlopens us from its own
// constructor, so every Tpf2mpPluginInit has returned before the game's static
// initialisers and main() run: the header's "before the exe entry point", held
// more strictly than the Windows loader thread could. That costs two things:
//   - main() waits for every init. An init that waits for anything the game does
//     (a window, a global, one of its own threads) hangs the game at start. On
//     Windows it only held up its own plugin. A watchdog thread, alive while
//     plugins load, logs a plugin still inside init after 10 s.
//   - init runs inside dlopen, under glibc's loader lock. The lock is recursive,
//     so dlopen, dlsym and dladdr on the init thread are fine, but an init that
//     waits for ANOTHER thread calling any of them deadlocks the game at start
//     (off-game test: dlsym on a second thread blocked until init returned).
//     Nothing in the host table takes that lock.
//
// LOG
// Each line is one write(2) on an O_APPEND descriptor: nothing is buffered,
// lines from threads (or from two games sharing the data folder) do not
// interleave, and no lock of ours is taken. ApiLog finds the calling plugin in a
// list it reads without locking, so a plugin may log from any thread, and from
// a signal handler as far as vsnprintf is safe there. errno survives the call.
//
// NO CRASH GUARD, NO CATCH
// host.cpp runs init under __try/__except. There is no safe equivalent here: a
// siglongjmp out of a SIGSEGV in foreign C++ skips its destructors and leaves
// its locks held, and a game carrying on in that state fails later somewhere
// unrelated. Nor can a C++ exception leaving init be caught. The unwinder
// raising it is the plugin's (its static libgcc, or the shared libgcc_s), and in
// phase 2 it runs this library's personality routine, whose _Unwind_SetGR is our
// own static copy working on a context the other copy built. Tried off-game with
// a catch (...) here (2026-09-12): a plugin with a static runtime aborted in
// _Unwind_SetGR.cold (host) <- __gxx_personality_v0 (host) <-
// _Unwind_RaiseException_Phase2 (plugin), and one on libstdc++.so.6 and
// libgcc_s.so.1 aborted in the same two host frames, after its destructors ran.
// So a fault or an exception in init ends the game (tpf2mp_plugin.h already
// rules exceptions out at this boundary), and the log line written before each
// call names the plugin that did it.
//
// TPF2MP_NO_PATCHES=1, the bridge's switch for telling a crash our patches cause
// from anything else, loads no plugins either: plugins patch code too.
//
// Process-lifetime state is leaked on purpose (H() below): a plugin's threads
// may still call the host while exit() runs static destructors.
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "cfg_linux.h"
#include "../codewrite_linux.h"
#include "datadir_linux.h"
#include "game_image.h"
#include "hook.h"
#include "plugin/tpf2mp_plugin.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Plugin {
    std::string stem;              // file name without .so: log prefix and config section
    void*       handle = nullptr;
    int         ncode = 0;         // its executable PT_LOAD ranges, for ApiLog's prefix
    uintptr_t   code[8][2] = {};
};

struct Host {
    int         logFd = -1;        // tpf2mp_host.log, O_APPEND
    std::string dataDir;           // with trailing '/'; set before any plugin loads
    uintptr_t   base = 0;          // executable's load address; 0 when not TransportFever2
    bool        buildOk = false;
    uintptr_t   imageLo = 0;       // PT_LOAD extent of the executable, as RVAs
    uintptr_t   imageHi = 0;
    int         memFd = -1;        // /proc/self/mem for patchBytes, opened before the first plugin loads
    pid_t       memPid = 0;        // the process it addresses: a forked child must not write through it
};

static Host& H()
{
    static Host* h = new Host;
    return *h;
}

// Every plugin whose init is called, in load order. Only the loader thread adds
// to it. Each record is complete before the count that exposes it grows, and none
// is changed or removed afterwards, so PrefixFor reads it without a lock: a
// plugin that logs from a signal handler cannot deadlock on the host.
static const size_t kMaxPlugins = 256;
static Plugin* g_plugins[kMaxPlugins];
static std::atomic<size_t> g_nplugins{0};

static std::atomic<const char*> g_curPlugin{"host"};   // the plugin whose init is running
static std::mutex g_patchMtx;   // the host's own patches one at a time (other libraries': PAGE PROTECTION RACES)

// What InitWatchdog sees of the init that is running.
static std::atomic<int>     g_initIdx{-1};      // its index in g_plugins; -1 between inits
static std::atomic<int64_t> g_initStartNs{0};   // CLOCK_MONOTONIC when it was called; set before g_initIdx
static std::atomic<bool>    g_loadDone{false};

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
static void WriteLog(const char* s, size_t n)
{
    const int fd = H().logFd;
    if (fd < 0) return;
    while (n) {
        const ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

// A host line: the format carries its own "[prefix] " and "\n".
__attribute__((format(printf, 1, 2)))
static void LogRaw(const char* fmt, ...)
{
    if (H().logFd < 0) return;
    const int savedErrno = errno;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(buf)) { len = sizeof(buf) - 1; buf[len - 1] = '\n'; }   // cut, but still one line
        WriteLog(buf, len);
    }
    errno = savedErrno;
}

// The plugin whose code contains `ret`. A line from a plugin's own thread or
// hook, long after its init, still carries its name (host.cpp could only name
// the plugin whose init was running). A call from anywhere else -- a tail call,
// a stub the plugin generated -- falls back to that, then to "host".
static const char* PrefixFor(uintptr_t ret)
{
    const size_t n = g_nplugins.load();
    for (size_t k = 0; k < n; k++) {
        const Plugin* p = g_plugins[k];
        for (int i = 0; i < p->ncode; i++)
            if (ret >= p->code[i][0] && ret < p->code[i][1]) return p->stem.c_str();
    }
    return g_curPlugin.load();
}

// ---------------------------------------------------------------------------
// The page map
// ---------------------------------------------------------------------------
struct Mapping { uintptr_t start, end; int prot; };

// /proc/self/maps, parsed: one mapping per line, sorted, not overlapping. Read
// fresh on every call: plugins verify and patch a handful of sites at start,
// and the map is only true at the moment it is read. The text goes straight
// into the heap: callers include plugin threads and hook detours with small
// stacks. Returns false when the file cannot be read, which is logged once:
// every call that needs the map then refuses, and without that line a plugin's
// "wrong game build" would be the only trace.
static bool ReadMaps(std::vector<Mapping>& out)
{
    out.clear();
    std::string text;
    int err = 0;
    int fd;
    do fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        err = errno;
    } else {
        static const size_t kChunk = 65536;
        size_t used = 0;
        for (;;) {
            text.resize(used + kChunk);
            const ssize_t n = read(fd, &text[used], kChunk);
            if (n > 0) { used += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) err = errno;
            break;
        }
        text.resize(used);
        close(fd);
    }
    if (err) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            LogRaw("[host] /proc/self/maps unreadable (%s) -- verifyBytes, patchBytes and installHook refuse\n",
                   strerror(err));
        return false;
    }
    // "start-end perms offset dev inode path"
    const char* p = text.c_str();
    const char* const end = p + text.size();
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;
        char* q = nullptr;
        const uintptr_t s = (uintptr_t)strtoull(p, &q, 16);
        if (q < nl && *q == '-') {
            const uintptr_t e = (uintptr_t)strtoull(q + 1, &q, 16);
            if (q + 4 <= nl && *q == ' ') {
                Mapping m = { s, e, 0 };
                if (q[1] == 'r') m.prot |= PROT_READ;
                if (q[2] == 'w') m.prot |= PROT_WRITE;
                if (q[3] == 'x') m.prot |= PROT_EXEC;
                out.push_back(m);
            }
        }
        p = nl + 1;
    }
    return true;
}

// Is every byte of [addr, addr+len) mapped with at least `need`? The range may
// span adjacent mappings; a hole or a weaker mapping anywhere fails it.
static bool Covered(const std::vector<Mapping>& maps, uintptr_t addr, size_t len, int need)
{
    if (!len || addr + len < addr) return false;
    uintptr_t cur = addr;
    const uintptr_t end = addr + len;
    for (const Mapping& m : maps) {
        if (m.end <= cur) continue;
        if (m.start > cur || (m.prot & need) != need) return false;
        cur = m.end;
        if (cur >= end) return true;
    }
    return false;
}

// moduleBase()+rva when [rva, rva+len) lies inside the executable's PT_LOAD
// extent, else 0.
static uintptr_t ImageAddr(uintptr_t rva, uint32_t len)
{
    const Host& h = H();
    if (!h.base || !len) return 0;
    if (rva < h.imageLo || rva >= h.imageHi || len > h.imageHi - rva) return 0;
    return h.base + rva;
}

// ---------------------------------------------------------------------------
// Host API implementation
// ---------------------------------------------------------------------------
// The message is cut at 1023 bytes, then prefixed; a newline inside it starts
// a line without a prefix.
__attribute__((format(printf, 1, 2)))
static void ApiLog(const char* fmt, ...)
{
    if (!fmt || H().logFd < 0) return;
    const int savedErrno = errno;
    const char* who = PrefixFor((uintptr_t)__builtin_return_address(0));
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    const int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n >= 0) {
        const size_t len = strlen(msg);
        const bool nl = len && msg[len - 1] == '\n';
        char line[sizeof(msg) + 300];   // '[' + a stem of at most 255 bytes + "] " + msg + '\n'
        const int m = snprintf(line, sizeof(line), "[%.255s] %s%s", who, msg, nl ? "" : "\n");
        if (m > 0) WriteLog(line, std::min((size_t)m, sizeof(line) - 1));   // one write: the line stays whole
    }
    errno = savedErrno;
}

static int         ApiCfgInt (const char* s, const char* k, int def)         { return tpf2mp::CfgInt(s, k, def); }
static int         ApiCfgBool(const char* s, const char* k, int def)         { return tpf2mp::CfgBool(s, k, def != 0) ? 1 : 0; }
static const char* ApiCfgStr (const char* s, const char* k, const char* def) { return tpf2mp::CfgStr(s, k, def); }
static const char* ApiDataDir(void)                                          { return H().dataDir.c_str(); }
static uintptr_t   ApiModuleBase(void)                                       { return H().base; }
static int         ApiBuildOk(void)                                          { return H().base && H().buildOk ? 1 : 0; }

static int ApiVerifyBytes(uintptr_t rva, const uint8_t* expected, uint32_t len)
{
    const uintptr_t p = expected ? ImageAddr(rva, len) : 0;
    if (!p) return 0;
    std::vector<Mapping> maps;
    if (!ReadMaps(maps) || !Covered(maps, p, len, PROT_READ)) return 0;   // an unreadable map: ReadMaps logged it
    return memcmp((void*)p, expected, len) == 0 ? 1 : 0;
}

static int ApiInstallHook(uintptr_t target, void* detour, int stealBytes, void** trampolineOut)
{
    const char* who = PrefixFor((uintptr_t)__builtin_return_address(0));
    const unsigned long at = (unsigned long)target;
    if (!target || !detour || !trampolineOut || stealBytes < 14 || stealBytes > 32) {
        LogRaw("[%s] installHook(%#lx, steal %d) refused: bad arguments\n", who, at, stealBytes);
        return 0;
    }
    std::lock_guard<std::mutex> lk(g_patchMtx);
    std::vector<Mapping> maps;
    if (!ReadMaps(maps)) {
        LogRaw("[%s] installHook(%#lx) refused: could not read /proc/self/maps\n", who, at);
        return 0;
    }
    if (!Covered(maps, target, (size_t)stealBytes, PROT_READ | PROT_EXEC)) {
        LogRaw("[%s] installHook(%#lx) refused: its %d bytes are not mapped readable and executable\n",
               who, at, stealBytes);
        return 0;
    }
    // The decoder reads at most 6 bytes of an instruction (66, REX, 0F, opcode,
    // ModRM, SIB), and the last instruction it reads starts before the cut.
    const unsigned char* const t = (const unsigned char*)target;
    if (!Covered(maps, target, (size_t)stealBytes + 5, PROT_READ)) {
        LogRaw("[%s] installHook(%#lx): the bytes just past the %d stolen ones are not mapped readable, so the "
               "prologue decoder was not run -- installed on the plugin's word\n", who, at, stealBytes);
    } else {
        const int cut = PrologueSteal(t, stealBytes);
        if (cut > stealBytes) {
            LogRaw("[%s] installHook(%#lx) refused: stealing %d bytes cuts an instruction (the next boundary is at %d)\n",
                   who, at, stealBytes, cut);
            return 0;
        } else if (cut == 0) {
            LogRaw("[%s] installHook(%#lx): the prologue decoder does not know every instruction in the %d stolen bytes "
                   "(or one is RIP-relative) -- installed on the plugin's word\n", who, at, stealBytes);
        }
    }
    // The shared hook writer leaves existing game page protections untouched.
    const bool ok = InstallHook(target, detour, stealBytes, trampolineOut);
    if (ok) LogRaw("[%s] installHook(%#lx, steal %d): installed\n", who, at, stealBytes);
    else    LogRaw("[%s] installHook(%#lx) FAILED (trampoline allocation or code write)\n", who, at);
    return ok ? 1 : 0;
}

static int ApiPatchBytes(uintptr_t rva, const uint8_t* bytes, uint32_t len)
{
    const char* who = PrefixFor((uintptr_t)__builtin_return_address(0));
    const unsigned long r = (unsigned long)rva;
    const uintptr_t p = bytes ? ImageAddr(rva, len) : 0;
    if (!p) {
        LogRaw("[%s] patchBytes(rva %#lx, %u bytes) refused: not inside the game image\n", who, r, len);
        return 0;
    }
    std::lock_guard<std::mutex> lk(g_patchMtx);
    std::vector<Mapping> maps;
    if (!ReadMaps(maps)) {
        LogRaw("[%s] patchBytes(rva %#lx, %u bytes) refused: could not read /proc/self/maps\n", who, r, len);
        return 0;
    }
    if (!Covered(maps, p, len, PROT_READ)) {
        LogRaw("[%s] patchBytes(rva %#lx, %u bytes) refused: not mapped readable\n", who, r, len);
        return 0;
    }

    // 1. Through /proc/self/mem, with no protection change (codewrite_linux.h).
    const Host& h = H();
    const bool ownMem = h.memFd >= 0 && getpid() == h.memPid;
    if (ownMem) {
        int err = 0;
        switch (Tpf2mpCodeWrite(h.memFd, p, bytes, len, &err)) {
        case TPF2MP_CW_OK:
            LogRaw("[%s] patchBytes(rva %#lx, %u bytes): written through /proc/self/mem\n", who, r, len);
            return 1;
        case TPF2MP_CW_PARTIAL_RESTORED:
            LogRaw("[%s] patchBytes(rva %#lx, %u bytes) FAILED: /proc/self/mem took only part of the bytes (%s); "
                   "the original bytes are back\n", who, r, len, strerror(err));
            return 0;
        case TPF2MP_CW_PARTIAL_BROKEN:
            LogRaw("[%s] patchBytes(rva %#lx, %u bytes) FAILED: /proc/self/mem took only part of the bytes (%s), "
                   "and writing the original bytes back failed -- the range is left HALF-PATCHED\n",
                   who, r, len, strerror(err));
            return 0;
        case TPF2MP_CW_READBACK:
            if (err)
                LogRaw("[%s] patchBytes(rva %#lx, %u bytes) FAILED: written through /proc/self/mem, "
                       "but reading it back failed (%s)\n", who, r, len, strerror(err));
            else
                LogRaw("[%s] patchBytes(rva %#lx, %u bytes) FAILED: written through /proc/self/mem, "
                       "but other bytes read back (another writer?)\n", who, r, len);
            return 0;
        default:   // TPF2MP_CW_UNAVAILABLE: nothing was written
            LogRaw("[%s] patchBytes(rva %#lx, %u bytes): /proc/self/mem refused the write (%s) -- "
                   "patch refused\n", who, r, len, strerror(err));
            break;
        }
    }

    LogRaw("[%s] patchBytes(rva %#lx, %u bytes) refused: no usable self-memory writer\n",
           who, r, len);
    return 0;
}

static const Tpf2mpHost g_api = {
    (uint32_t)sizeof(Tpf2mpHost),
    TPF2MP_ABI_MAJOR,
    ApiLog,
    ApiCfgInt, ApiCfgBool, ApiCfgStr,
    ApiModuleBase, ApiBuildOk, ApiVerifyBytes,
    ApiInstallHook, ApiPatchBytes,
    ApiDataDir,
};

// ---------------------------------------------------------------------------
// Plugin discovery
// ---------------------------------------------------------------------------
struct Found { std::string path; std::string leaf; };

// Regular files (or links to them) named *.so, in byte order. A leaf already
// found in an earlier folder is skipped: that is how a user's copy in the data
// folder hides a shipped one without deleting it.
static void ScanDir(const std::string& dir, std::vector<Found>& out)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const size_t n = strlen(e->d_name);
        if (n > 3 && memcmp(e->d_name + n - 3, ".so", 3) == 0) names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& leaf : names) {
        const std::string path = dir + leaf;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        bool dup = false;
        for (const Found& f : out)
            if (f.leaf == leaf) { dup = true; break; }
        if (!dup) out.push_back(Found{ path, leaf });
    }
}

// Appends `dir` unless it is empty or already listed under this or another name.
static void AddDir(std::vector<std::string>& dirs, const std::string& dir)
{
    if (dir.empty()) return;
    char a[PATH_MAX], b[PATH_MAX];
    const bool resolved = realpath(dir.c_str(), a) != nullptr;
    for (const std::string& d : dirs) {
        if (d == dir) return;
        if (resolved && realpath(d.c_str(), b) && strcmp(a, b) == 0) return;
    }
    dirs.push_back(dir);
}

static std::string DirOf(const char* path)
{
    const char* slash = path ? strrchr(path, '/') : nullptr;
    return slash ? std::string(path, (size_t)(slash - path) + 1) : std::string();
}

// The executable segments of the object that holds `addr` (Tpf2mpPluginInit).
static int CodeRangesCb(dl_phdr_info* info, size_t, void* data)
{
    auto* p = static_cast<Plugin*>(data);
    const uintptr_t addr = p->code[0][0];
    int n = 0;
    uintptr_t ranges[8][2] = {};
    bool hit = false;
    for (int i = 0; i < info->dlpi_phnum && n < 8; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
        ranges[n][0] = info->dlpi_addr + ph.p_vaddr;
        ranges[n][1] = ranges[n][0] + ph.p_memsz;
        hit = hit || (addr >= ranges[n][0] && addr < ranges[n][1]);
        n++;
    }
    if (!hit) return 0;
    memcpy(p->code, ranges, sizeof(ranges));
    p->ncode = n;
    return 1;
}

// The PT_LOAD extent of the executable, which dl_iterate_phdr reports first.
static int ImageExtentCb(dl_phdr_info* info, size_t, void* data)
{
    auto* h = static_cast<Host*>(data);
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        lo = std::min<uintptr_t>(lo, ph.p_vaddr);
        hi = std::max<uintptr_t>(hi, ph.p_vaddr + ph.p_memsz);
    }
    if (hi > lo) { h->imageLo = lo; h->imageHi = hi; }
    return 1;
}

static bool IsGameProcess(std::string& exePath)
{
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    exePath = exe;
    const char* base = strrchr(exe, '/');
    return base && strcmp(base + 1, "TransportFever2") == 0;
}

static const char* ResultName(int rc)
{
    switch (rc) {
    case TPF2MP_OK:           return "OK";
    case TPF2MP_ERR_ABI:      return "ABI MISMATCH";
    case TPF2MP_ERR_BUILD:    return "WRONG GAME BUILD";
    case TPF2MP_ERR_DISABLED: return "disabled in config";
    default:                  return "FAILED";
    }
}

static int64_t MonoNs()
{
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

// One line for each plugin still inside init after 10 s: main() cannot start
// until it returns (WHEN PLUGINS RUN), and a "calling" line is otherwise the
// only trace of a hang. Runs only while plugins load. It calls nothing that
// takes the loader lock the init thread holds: nanosleep, the clock and write.
// g_initIdx is read on both sides of g_initStartNs, which the loader sets first,
// so the time read belongs to the plugin named.
static void* InitWatchdog(void*)
{
    static const int64_t kWarnNs = 10LL * 1000000000LL;
    const timespec tick = { 0, 250L * 1000L * 1000L };
    int warned = -1;
    while (!g_loadDone.load()) {
        const int idx = g_initIdx.load();
        if (idx >= 0 && idx != warned) {
            const int64_t start = g_initStartNs.load();
            if (g_initIdx.load() == idx && MonoNs() - start >= kWarnNs) {
                LogRaw("[host] %s: still inside Tpf2mpPluginInit after 10 s -- the game cannot start until it returns\n",
                       g_plugins[idx]->stem.c_str());
                warned = idx;
            }
        }
        nanosleep(&tick, nullptr);
    }
    return nullptr;
}

static void StartInitWatchdog()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    const int rc = pthread_create(&th, &attr, InitWatchdog, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        LogRaw("[host] no init watchdog (%s): a plugin that hangs in init will not be named\n", strerror(rc));
}

// Opened before any plugin loads: a plugin that makes the process non-dumpable
// makes /proc/self/mem root's, and a descriptor opened earlier keeps working
// (codewrite_linux.h).
static void OpenSelfMem()
{
    Host& h = H();
    h.memFd = Tpf2mpOpenSelfMem();
    const int err = errno;
    h.memPid = getpid();
    if (h.memFd >= 0)
        LogRaw("[host] patchBytes writes through /proc/self/mem (no page protection changes)\n");
    else
        LogRaw("[host] /proc/self/mem not opened (%s) -- patchBytes changes page protections, "
               "which can race the other libraries' patches\n", strerror(err));
}

static void LoadPlugins(const std::vector<std::string>& dirs)
{
    std::vector<Found> found;
    std::string looked;
    for (const std::string& d : dirs) {
        ScanDir(d + "plugins/", found);
        looked += (looked.empty() ? "" : ", ") + d + "plugins/";
    }
    if (found.empty()) {
        LogRaw("[host] no plugins found (looked in %s)\n", looked.c_str());
        return;
    }
    LogRaw("[host] %zu plugin(s) found\n", found.size());
    OpenSelfMem();
    StartInitWatchdog();

    std::vector<std::pair<void*, std::string> > opened;   // handle -> the leaf that opened it
    for (const Found& f : found) {
        // The config section is the file name without .so, so "enabled=0" can
        // switch one off without deleting it -- checked BEFORE dlopen, because a
        // disabled plugin's constructors must not run either.
        const std::string stem = f.leaf.substr(0, f.leaf.size() - 3);

        // plugins/<stem>.cfg beside the .so is merged over tpf2mp.cfg first, so
        // it can carry enabled=0 too (host.cpp: another installer's settings).
        const std::string pcfg = f.path.substr(0, f.path.size() - 3) + ".cfg";
        int cfgErr = 0;
        if (tpf2mp::CfgMergeFile(pcfg.c_str(), &cfgErr))
            LogRaw("[host] %s: merged %s\n", stem.c_str(), pcfg.c_str());
        else if (cfgErr != ENOENT)
            LogRaw("[host] %s: could not read %s (%s) -- its settings, including enabled, are ignored\n",
                   stem.c_str(), pcfg.c_str(), strerror(cfgErr));

        if (!tpf2mp::CfgBool(stem.c_str(), "enabled", true)) {
            LogRaw("[host] %s: skipped (enabled=0)\n", stem.c_str());
            continue;
        }
        if (g_nplugins.load() >= kMaxPlugins) {
            LogRaw("[host] %s: not loaded -- the host takes at most %zu plugins\n", f.leaf.c_str(), kMaxPlugins);
            continue;
        }

        dlerror();
        void* h = dlopen(f.path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            const char* err = dlerror();
            LogRaw("[host] %s: dlopen FAILED: %s\n", f.leaf.c_str(), err ? err : "(no reason given)");
            continue;
        }
        // glibc hands back the same handle for a file it already has open under
        // another name (a link): one library, one init.
        const std::string* sameAs = nullptr;
        for (const auto& o : opened)
            if (o.first == h) { sameAs = &o.second; break; }
        if (sameAs) {
            LogRaw("[host] %s: the same library as %s -- not initialised twice\n", f.leaf.c_str(), sameAs->c_str());
            continue;
        }
        opened.push_back({ h, f.leaf });

        dlerror();
        const auto init = (Tpf2mpPluginInitFn)dlsym(h, "Tpf2mpPluginInit");
        if (!init) {
            LogRaw("[host] %s: no Tpf2mpPluginInit export -- not a plugin, left loaded\n", f.leaf.c_str());
            continue;
        }
        // dlsym(handle) searches the library's dependencies as well. A library
        // linked against another plugin, with no init of its own, would get that
        // plugin's, which would then run a second time under this name.
        link_map* own = nullptr;
        link_map* from = nullptr;
        Dl_info di = {};
        if (dlinfo(h, RTLD_DI_LINKMAP, &own) != 0 || !own
            || !dladdr1((void*)init, &di, (void**)&from, RTLD_DL_LINKMAP) || !from) {
            LogRaw("[host] %s: cannot tell which library its Tpf2mpPluginInit is in -- not called, left loaded\n",
                   f.leaf.c_str());
            continue;
        }
        if (from != own) {
            LogRaw("[host] %s: Tpf2mpPluginInit comes from a dependency (%s) -- not called, left loaded\n",
                   f.leaf.c_str(), di.dli_fname && di.dli_fname[0] ? di.dli_fname : "?");
            continue;
        }
        auto* rec = new Plugin;   // never freed: ApiLog may name it at any time
        rec->stem = stem;
        rec->handle = h;
        rec->code[0][0] = (uintptr_t)init;
        dl_iterate_phdr(CodeRangesCb, rec);
        if (rec->ncode == 0) {
            LogRaw("[host] %s: Tpf2mpPluginInit is not in an executable segment -- not called, left loaded\n",
                   f.leaf.c_str());
            continue;
        }
        const size_t idx = g_nplugins.load();
        g_plugins[idx] = rec;           // complete before the count exposes it
        g_nplugins.store(idx + 1);

        // Before and after: a plugin that faults, throws or hangs in init is the
        // last one with a "calling" line and no result.
        LogRaw("[host] %s: calling Tpf2mpPluginInit (%s)\n", stem.c_str(), f.path.c_str());
        Tpf2mpPluginInfo info;
        memset(&info, 0, sizeof(info));
        g_curPlugin = rec->stem.c_str();
        g_initStartNs = MonoNs();
        g_initIdx = (int)idx;
        const int rc = init(&g_api, &info);
        g_initIdx = -1;
        g_curPlugin = "host";
        LogRaw("[host] %s: %s%s%s -> %s (%d)\n", stem.c_str(),
               info.name ? info.name : stem.c_str(),
               info.version ? " " : "", info.version ? info.version : "",
               ResultName(rc), rc);
        if (info.summary) LogRaw("[host]   %s\n", info.summary);
    }
    g_loadDone = true;
}

// ---------------------------------------------------------------------------
__attribute__((constructor))
static void HostLoad()
{
    Host& h = H();
    char dataDir[4096];
    if (!Tpf2mpDataDirA(dataDir, sizeof(dataDir))) {
        // No log file without a data folder, and plugins are promised one.
        fprintf(stderr, "[tpf2_pluginhost] no data folder (TPF2MP_DATADIR, XDG_DATA_HOME, HOME) -- no plugins\n");
        return;
    }
    h.dataDir = dataDir;
    // O_APPEND: two games sharing the data folder both append whole lines.
    // O_CLOEXEC: the lobby the menu starts does not inherit it.
    const std::string logPath = h.dataDir + "tpf2mp_host.log";
    do h.logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    while (h.logFd < 0 && errno == EINTR);

    std::string exePath;
    const bool inGame = IsGameProcess(exePath);
    const Tpf2GameImage img = Tpf2mpGameImage();
    dl_iterate_phdr(ImageExtentCb, &h);
    h.base = inGame ? img.base : 0;
    h.buildOk = inGame && img.buildOk;

    Dl_info self = {};
    dladdr((void*)&HostLoad, &self);
    const std::string libDir = DirOf(self.dli_fname);
    const std::string gameDir = DirOf(exePath.c_str());

    std::vector<std::string> cfgDirs;
    AddDir(cfgDirs, libDir);
    AddDir(cfgDirs, gameDir);
    AddDir(cfgDirs, h.dataDir);
    std::vector<std::pair<std::string, int> > unreadable;
    const std::string& cfg = tpf2mp::CfgLoad(cfgDirs, &unreadable);

    LogRaw("[host] pid=%d abi=%d\n", (int)getpid(), TPF2MP_ABI_MAJOR);
    LogRaw("[host] data dir: %s\n", h.dataDir.c_str());
    LogRaw("[host] lib dir: %s  game dir: %s\n", libDir.empty() ? "?" : libDir.c_str(),
           gameDir.empty() ? "?" : gameDir.c_str());
    for (const auto& u : unreadable)
        LogRaw("[host] could not read %s (%s) -- skipped\n", u.first.c_str(), strerror(u.second));
    LogRaw("[host] config: %s\n", cfg.empty() ? "(none found -- built-in defaults)" : cfg.c_str());
    LogRaw("[host] game build: %s\n",
           !inGame ? "not TransportFever2 -- plugins that patch will refuse"
                   : (h.buildOk ? "MATCHES 35924" : "MISMATCH -- plugins that patch will refuse"));

    const char* noPatches = getenv("TPF2MP_NO_PATCHES");
    if (noPatches && noPatches[0] == '1') {
        LogRaw("[host] TPF2MP_NO_PATCHES=1: no plugins loaded\n");
        return;
    }

    std::vector<std::string> pluginDirs;
    AddDir(pluginDirs, h.dataDir);
    AddDir(pluginDirs, libDir);
    AddDir(pluginDirs, gameDir);
    LoadPlugins(pluginDirs);
    LogRaw("[host] done\n");
}
