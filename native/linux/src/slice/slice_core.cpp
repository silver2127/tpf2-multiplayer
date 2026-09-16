// slice_core.cpp -- tpf2_slice.so's services for the decoder areas: the log, the
// cfg, the identity and session checks, the inject writer, guarded memory reads,
// libstdc++ container readers, and the factory, script-caller and armable call-site
// tables with their checks against the image.
// slice_hook.cpp lines 1-500 are the Windows original; each rule's incident is
// commented there and only the POSIX mechanics differ here. The facts behind the
// tables are in docs/re/linux/SLICE_CORE.md.
//
// Differences from Windows, each deliberate:
//   - One instance per DATA DIR, not per login session: flock() on
//     <data dir>tpf2_slice.lock replaces the named mutex. Two games sharing a data
//     dir behave as on Windows (the second stays inert); two data dirs
//     (TPF2MP_DATADIR) are two independent peers.
//   - The cfg matches Windows Lua: the game folder (the cwd),
//     then the data dir, never from the folder of this library (C-FILE-5: the
//     Lua's order).
//   - Every inject record is ONE write(2), its ARMED line included (C-FILE-4):
//     Windows' separate fprintf calls could interleave with the mod's appends.
//   - VirtualQuery + SEH become process_vm_readv on our own pid, or a pipe write,
//     whichever passes a self-test at init (C-READ-1, C-READ-2).
//   - The identity refusal and the session state are logged when they change, not
//     on every check.
#include "slice_core_internal.h"
#include "hook.h"
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>

static SliceCoreEnv g_env;
static char g_dataDir[4096];
static char g_rootDir[4096];
static int g_logFd = -1;
static int g_lockFd = -1;
static pid_t g_pid = 0;

static pid_t Pid()
{
    if (!g_pid) g_pid = getpid();
    return g_pid;
}

static bool WriteAll(int fd, const char* p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

// ---- image, log, time -------------------------------------------------------------

uintptr_t SliceImageBase() { return g_env.base; }

uintptr_t SliceRvaOf(uintptr_t addr)
{
    // The image spans at most its highest executable segment; RVAs of interest lie
    // below it. A pointer below the base is not in the image.
    if (!g_env.base || addr < g_env.base) return UINTPTR_MAX;
    uintptr_t hi = g_env.base;
    for (int i = 0; i < g_env.nExec; i++) if (g_env.exec[i].hi > hi) hi = g_env.exec[i].hi;
    return addr < hi ? addr - g_env.base : UINTPTR_MAX;
}

const SliceCoreEnv& SliceCoreEnvGet() { return g_env; }

bool SliceInExec(uintptr_t addr)
{
    for (int i = 0; i < g_env.nExec; i++)
        if (addr >= g_env.exec[i].lo && addr < g_env.exec[i].hi) return true;
    return false;
}

void SliceLog(const char* fmt, ...)
{
    if (g_logFd < 0) return;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (size_t)n;
    if (len >= sizeof(buf)) {
        memcpy(buf + sizeof(buf) - 5, "...\n", 4);
        len = sizeof(buf) - 1;
    }
    WriteAll(g_logFd, buf, len);
}

// syscall rather than gettid(): no dependency on the glibc version inside the
// Steam runtime container.
int SliceTid() { return (int)syscall(SYS_gettid); }

uint64_t SliceNowMs()
{
    timespec t = {};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

const char* SliceDataDir() { return g_dataDir; }

SliceOpenResult SliceCoreOpen(const SliceCoreEnv& env)
{
    g_env = env;
    snprintf(g_dataDir, sizeof(g_dataDir), "%s", env.dataDir ? env.dataDir : "");
    snprintf(g_rootDir, sizeof(g_rootDir), "%s", env.rootDir ? env.rootDir : "");
    g_env.dataDir = g_dataDir;
    g_env.rootDir = g_rootDir;
    g_pid = getpid();

    char lockPath[4200], logPath[4200];
    snprintf(lockPath, sizeof(lockPath), "%stpf2_slice.lock", g_dataDir);
    snprintf(logPath, sizeof(logPath), "%stpf2_slice.log", g_dataDir);

    // The lock is taken BEFORE the log is opened: truncating it would wipe the log
    // of the game that holds the lock. O_CLOEXEC: a child (the lobby) must not
    // inherit it and keep it after the game exits. Held for the process lifetime.
    int lockErr = 0;
    int fd = open(lockPath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOCTTY, 0600);
    if (fd < 0) {
        lockErr = errno;
    } else if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        lockErr = errno;
        close(fd);
        fd = -1;
        if (lockErr == EWOULDBLOCK) {
            // Say so in the running instance's log (append only, never create or truncate).
            int lf = open(logPath, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOCTTY);
            if (lf >= 0) {
                char line[256];
                int n = snprintf(line, sizeof(line),
                                 "[slice] pid %d: another game holds the slice lock on this data dir -- that one stays inert\n",
                                 (int)g_pid);
                if (n > 0) WriteAll(lf, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
                close(lf);
            }
            return SliceOpenResult::Locked;
        }
    }
    g_lockFd = fd;

    char keepPath[4096 + 64];
    snprintf(keepPath, sizeof(keepPath), "%stpf2mp_keep_logs.txt", g_dataDir);
    const bool keepLogs = access(keepPath, F_OK) == 0;
    g_logFd = open(logPath, O_WRONLY | O_CREAT | (keepLogs ? 0 : O_TRUNC) | O_APPEND | O_CLOEXEC | O_NOCTTY, 0644);
    if (keepLogs && g_logFd >= 0) SliceLog("\n==== slice session %ld pid %d (keeping logs) ====\n", (long)time(nullptr), (int)g_pid);
    if (g_logFd < 0) return SliceOpenResult::NoLog;   // nowhere to log: stay inert, as Windows does
    if (g_lockFd < 0) {
        SliceLog("[slice] no instance lock (%s): hooks stay disabled\n", strerror(lockErr));
        return SliceOpenResult::NoLock;
    }
    return SliceOpenResult::Ok;
}

// ---- cfg ---------------------------------------------------------------------------

static FILE* OpenRegular(const char* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return nullptr;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return nullptr; }
    FILE* f = fdopen(fd, "r");
    if (!f) close(fd);
    return f;
}

// Match unchanged Windows 0.4.22 Lua: game folder (cwd), then data dir.
static FILE* OpenCfg()
{
    char p[4200];
    if (FILE* f = OpenRegular("tpf2_slice.cfg")) return f;
    if (g_dataDir[0]) {
        snprintf(p, sizeof(p), "%stpf2_slice.cfg", g_dataDir);
        return OpenRegular(p);
    }
    return nullptr;
}

bool SliceCfgFlag(const char* key, bool def)
{
    FILE* f = OpenCfg();
    if (!f) return def;
    const size_t klen = strlen(key);
    bool val = def;
    bool lineStart = true;            // does the next fgets chunk begin a line?
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const size_t len = strlen(line);
        const bool endsLine = len > 0 && line[len - 1] == '\n';
        const bool startsLine = lineStart;
        lineStart = endsLine;
        // A chunk that neither ends its line nor the file is a line too long for
        // the buffer: its tail was not seen, so it cannot be exact.
        if (!startsLine || !(endsLine || feof(f))) continue;
        if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
        const char v = line[klen + 1];
        if (v != '0' && v != '1') continue;
        const char* rest = line + klen + 2;
        while (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\n') rest++;
        if (*rest) continue;
        val = (v == '1');
    }
    fclose(f);
    return val;
}

bool SliceDumpPropOn()
{
    static std::atomic<uint64_t> lastRead{0};
    static std::atomic<bool> on{false};
    const uint64_t now = SliceNowMs();
    const uint64_t last = lastRead.load(std::memory_order_relaxed);
    if (last && now - last < 2000) return on.load(std::memory_order_relaxed);
    lastRead.store(now ? now : 1, std::memory_order_relaxed);
    const bool v = SliceCfgFlag("dumpprop", false);
    on.store(v, std::memory_order_relaxed);
    return v;
}

// ---- identity and session ------------------------------------------------------------

static pthread_mutex_t g_noteMu = PTHREAD_MUTEX_INITIALIZER;
static char g_lastIdNote[256];

// Logs an identity problem once per distinct message (SessionLive asks every second).
static void NoteIdentity(const char* msg)
{
    pthread_mutex_lock(&g_noteMu);
    const bool changed = strcmp(msg, g_lastIdNote) != 0;
    if (changed) snprintf(g_lastIdNote, sizeof(g_lastIdNote), "%s", msg);
    pthread_mutex_unlock(&g_noteMu);
    if (changed && msg[0]) SliceLog("[slice] %s\n", msg);
}

bool SliceInstance(char* out, size_t cap)
{
    if (cap) out[0] = 0;
    if (!g_dataDir[0] || cap < 2) return false;
    char path[4200];
    snprintf(path, sizeof(path), "%stpf2_instance.txt", g_dataDir);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) { NoteIdentity("no identity file yet (tpf2_instance.txt) -- no instance letter"); return false; }
    char text[512];
    size_t n = 0;
    while (n < sizeof(text) - 1) {
        ssize_t r = read(fd, text + n, sizeof(text) - 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    text[n] = 0;

    // Line 1 is the letter. It names files, so only a short token of ASCII
    // letters and digits is accepted.
    size_t k = 0;
    while (k < n && text[k] != '\r' && text[k] != '\n' && text[k] != ' ') k++;
    char letter[8] = "";
    bool okLetter = k >= 1 && k <= 7;
    for (size_t i = 0; okLetter && i < k; i++) {
        const char c = text[i];
        okLetter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    }
    if (!okLetter) { NoteIdentity("identity file has no valid letter on line 1 -- no instance letter"); return false; }
    memcpy(letter, text, k);
    letter[k] = 0;

    // pid= on a line of its own start, bound to THIS process (slice_hook.cpp
    // ReadInstance: a file that fell through from another peer must not lend us
    // its letter). A port= line may or may not follow (C-FILE-2).
    unsigned long want = 0;
    bool havePid = false;
    for (size_t i = 0; i < n && !havePid; i++) {
        if ((i == 0 || text[i - 1] == '\n') && strncmp(text + i, "pid=", 4) == 0)
            havePid = sscanf(text + i, "pid=%lu", &want) == 1;
    }
    char msg[256];
    if (!havePid) {
        snprintf(msg, sizeof(msg), "identity file has no pid line -- refusing instance letter '%s' (mine pid=%d)",
                 letter, (int)Pid());
        NoteIdentity(msg);
        return false;
    }
    if (want != (unsigned long)Pid()) {
        snprintf(msg, sizeof(msg), "identity file pid=%lu != mine %d -- refusing instance letter '%s' "
                 "(another game's file?)", want, (int)Pid(), letter);
        NoteIdentity(msg);
        return false;
    }
    snprintf(msg, sizeof(msg), "instance letter '%s' (pid %d)", letter, (int)Pid());
    NoteIdentity(msg);
    if (k + 1 > cap) return false;
    memcpy(out, letter, k + 1);
    return true;
}

static pthread_mutex_t g_sessMu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_sessAt = 0;
static bool g_sessLive = false;
static int g_sessLogged = -1;

void SliceSessionCacheReset()
{
    pthread_mutex_lock(&g_sessMu);
    g_sessAt = 0;
    pthread_mutex_unlock(&g_sessMu);
}

static bool ComputeSessionLive(const char** why)
{
    // A timeout is uncertainty, never permission to execute a local action.
    const bool held = g_sessLive;
    char letter[8];
    if (!SliceInstance(letter, sizeof(letter))) { *why = "no instance letter"; return held; }
    char path[4200];
    snprintf(path, sizeof(path), "%slockstep_status_%s.txt", g_dataDir, letter);

    // Freshness first: a stale file is a mod that is not running (or a save
    // loaded without it). The Lua writes it every ~2.8 s; 15 s as on Windows.
    struct stat st;
    if (stat(path, &st) != 0) { *why = "no status file"; return held; }
    timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    const long double mt = (long double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9L;
    const long double nt = (long double)now.tv_sec + now.tv_nsec / 1e9L;
    if (nt < mt) { *why = "status file from the future"; return held; }
    if ((nt - mt) * 1000.0L > 15000.0L) { *why = "status file older than 15 s"; return held; }

    // A peer heartbeat or the lobby's player count establishes a live session.
    // 0.4.22: mp>=2 already counts before the first peer heartbeat arrives.
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) { *why = "status file unreadable"; return held; }
    char line[256];
    ssize_t r;
    do { r = read(fd, line, sizeof(line) - 1); } while (r < 0 && errno == EINTR);
    close(fd);
    if (r <= 0) { *why = "status file empty"; return held; }
    line[r] = 0;
    if (char* nl = strchr(line, '\n')) *nl = 0;
    const char* pk = strstr(line, "peer=");
    double pt = 0.0;
    const char* mk = strstr(line, "  mp=");
    int players = 0;
    int peerEnd = 0, rosterEnd = 0;
    const bool peerNumber = pk && sscanf(pk + 5, "%lf%n", &pt, &peerEnd) == 1 &&
        std::isfinite(pt) && pt >= 0 && (!pk[5 + peerEnd] || pk[5 + peerEnd] == ' ');
    const bool peerUnknown = pk && pk[5] == '?' && (!pk[6] || pk[6] == ' ');
    const bool haveRoster = mk && sscanf(mk + 5, "%d%n", &players, &rosterEnd) == 1 && players >= 0 && players <= 8;
    if ((peerNumber && pt > 0.0) || (haveRoster && players >= 2)) { *why = ""; return true; }
    // The original Lua truncates this file in place and ends with mp=N (no
    // newline). A partial prefix must not look like a new single-player world.
    const char* tail = haveRoster ? mk + 5 + rosterEnd : nullptr;
    if (tail) while (*tail == ' ' || *tail == '\t' || *tail == '\r') ++tail;
    if (haveRoster && tail && !*tail && (peerNumber || peerUnknown)) {
        *why = "complete fresh solo status"; return false;
    }
    *why = "incomplete status; retaining multiplayer state";
    return held;
}

bool SliceSessionLive()
{
    const uint64_t now = SliceNowMs();
    pthread_mutex_lock(&g_sessMu);
    if (g_sessAt && now - g_sessAt < 1000) {
        const bool v = g_sessLive;
        pthread_mutex_unlock(&g_sessMu);
        return v;
    }
    const char* why = "";
    const bool live = ComputeSessionLive(&why);
    g_sessLive = live;
    g_sessAt = now ? now : 1;
    const bool changed = g_sessLogged != (int)live;
    g_sessLogged = (int)live;
    pthread_mutex_unlock(&g_sessMu);
    if (changed) {
        if (live) SliceLog("[slice] session live: peer or multiplayer roster in lockstep_status -- captures are cancelled and replayed\n");
        else SliceLog("[slice] session not live (%s) -- captures run natively\n", why);
    }
    return live;
}


// ---- inject records ---------------------------------------------------------------------

static bool RecordReserve(SliceRecord* r, size_t extra)
{
    if (r->failed) return false;
    if (r->len == SIZE_MAX || extra > SIZE_MAX - r->len - 1) { r->failed = true; return false; }
    const size_t need = r->len + extra + 1;
    if (need <= r->cap) return true;
    size_t cap = r->cap ? r->cap : 256;
    while (cap < need) { if (cap > SIZE_MAX / 2) { cap = need; break; } cap *= 2; }
    char* d = (char*)realloc(r->data, cap);
    if (!d) { r->failed = true; return false; }
    r->data = d;
    r->cap = cap;
    return true;
}

void SliceRecordAppend(SliceRecord* r, const char* s, size_t n)
{
    if (!RecordReserve(r, n)) return;
    memcpy(r->data + r->len, s, n);
    r->len += n;
    r->data[r->len] = 0;
}

void SliceRecordPrintf(SliceRecord* r, const char* fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    char small[512];
    const int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) {
        r->failed = true;
    } else if ((size_t)n < sizeof(small)) {
        SliceRecordAppend(r, small, (size_t)n);
    } else if (RecordReserve(r, (size_t)n)) {
        vsnprintf(r->data + r->len, (size_t)n + 1, fmt, ap2);
        r->len += (size_t)n;
    }
    va_end(ap2);
}

void SliceRecordFree(SliceRecord* r)
{
    free(r->data);
    r->data = nullptr;
    r->len = r->cap = 0;
    r->failed = false;
}

static SliceWriteFn g_writeFn = nullptr;
void SliceInjectSetWriteFn(SliceWriteFn fn) { g_writeFn = fn; }

static ssize_t InjectWriteRaw(int fd, const char* p, size_t n)
{
    ssize_t w;
    do { w = g_writeFn ? g_writeFn(fd, p, n) : write(fd, p, n); } while (w < 0 && errno == EINTR);
    return w;
}

SliceInjectResult SliceInjectWrite(const SliceRecord& r, SliceArmedLine armed)
{
    // Preserve release 0.4.22's exact capture format. Uncancelled captures are
    // refused: multiplayer must never fall back to a local-only application.
    if (armed == SliceArmedLine::Zero || r.failed || !r.data || !r.len)
        return SliceInjectResult::NotWritten;
    char letter[8], path[4200];
    if (!SliceInstance(letter, sizeof(letter))) return SliceInjectResult::NotWritten;
    snprintf(path, sizeof(path), "%slockstep_inject_%s.txt", g_dataDir, letter);
    const char* head = armed == SliceArmedLine::One ? "ARMED 1\n" : "";
    const size_t nh = strlen(head);
    if (r.len > SIZE_MAX - nh - 2) return SliceInjectResult::NotWritten;
    char* buf = (char*)malloc(nh + r.len + 2);
    if (!buf) return SliceInjectResult::NotWritten;
    memcpy(buf, head, nh); memcpy(buf + nh, r.data, r.len);
    size_t n = nh + r.len;
    if (buf[n - 1] != '\n') buf[n++] = '\n';
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) { free(buf); return SliceInjectResult::NotWritten; }
    const ssize_t written = InjectWriteRaw(fd, buf, n);
    auto result = written == (ssize_t)n ? SliceInjectResult::Written :
        written > 0 ? SliceInjectResult::Partial : SliceInjectResult::NotWritten;
    if (written > 0 && written < (ssize_t)n) {
        if ((size_t)written + 1 == n && InjectWriteRaw(fd, "\n", 1) == 1) result = SliceInjectResult::Written;
        else {
            const bool inHeader = (size_t)written < nh;
            const bool midLine = buf[written - 1] != '\n';
            const char* tail = midLine ? "\nARMED 0\n" : "ARMED 0\n";
            InjectWriteRaw(fd, tail, strlen(tail));
            if (inHeader) result = SliceInjectResult::NotWritten;
            SliceLog("[slice] short inject write (%zd/%zu); native command stays blocked\n", written, n);
        }
    }
    close(fd); free(buf);
    return result;
}

bool SliceInjectLine(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char small[1024];
    const int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(small)) {
        SliceLog("[slice] inject line too long or unformattable -- use a SliceRecord\n");
        return false;
    }
    SliceRecord r = {};
    SliceRecordAppend(&r, small, (size_t)n);
    const bool ok = SliceInjectWrite(r, SliceArmedLine::None) == SliceInjectResult::Written;
    SliceRecordFree(&r);
    return ok;
}

// ---- guarded reads -------------------------------------------------------------------------

static std::atomic<int> g_readMech{kSliceReadNone};
static pthread_mutex_t g_pipeMu = PTHREAD_MUTEX_INITIALIZER;
static int g_pipe[2] = { -1, -1 };

// Bytes read, or -errno. The kernel copies page by page and stops at the first
// page it cannot read (C-READ-1).
static ssize_t VmRaw(uintptr_t addr, void* out, size_t n)
{
    iovec local = { out, n };
    iovec remote = { (void*)addr, n };
    const ssize_t got = process_vm_readv(Pid(), &local, 1, &remote, 1, 0);
    return got < 0 ? -errno : got;
}

static void DrainPipeLocked()
{
    char tmp[4096];
    while (read(g_pipe[0], tmp, sizeof(tmp)) > 0) {}
}

// The pipe probe: write() copies from `addr` in the kernel and fails with EFAULT
// instead of faulting (C-READ-1). Chunks of at most PIPE_BUF into an empty
// non-blocking pipe, read straight back. Bytes read, or -errno.
static ssize_t PipeRaw(uintptr_t addr, void* out, size_t n)
{
    pthread_mutex_lock(&g_pipeMu);
    if (g_pipe[0] < 0 && pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        const int e = errno;
        pthread_mutex_unlock(&g_pipeMu);
        return -e;
    }
    size_t off = 0;
    ssize_t result = 0;
    while (off < n) {
        const size_t chunk = n - off < 4096 ? n - off : 4096;
        ssize_t w;
        do { w = write(g_pipe[1], (const void*)(addr + off), chunk); } while (w < 0 && errno == EINTR);
        if (w < 0) { result = off ? (ssize_t)off : -errno; DrainPipeLocked(); break; }
        ssize_t r;
        do { r = read(g_pipe[0], (char*)out + off, (size_t)w); } while (r < 0 && errno == EINTR);
        if (r != w) { result = -EIO; DrainPipeLocked(); break; }
        off += (size_t)w;
        result = (ssize_t)off;
        if ((size_t)w != chunk) break;
    }
    pthread_mutex_unlock(&g_pipeMu);
    return result;
}

static ssize_t RawRead(int mech, uintptr_t addr, void* out, size_t n)
{
    if (mech == kSliceReadVm) return VmRaw(addr, out, n);
    if (mech == kSliceReadPipe) return PipeRaw(addr, out, n);
    return -ENOSYS;
}

void SliceReadSelect(int mech) { g_readMech.store(mech, std::memory_order_relaxed); }
int SliceReadMechanism() { return g_readMech.load(std::memory_order_relaxed); }
bool SliceReadsOk() { return SliceReadMechanism() != kSliceReadNone; }

bool SliceRead(uintptr_t addr, void* out, size_t n)
{
    if (n == 0) return true;
    if (addr < 0x1000 || addr + n < addr) return false;
    const int mech = SliceReadMechanism();
    if (mech == kSliceReadNone) return false;
    return RawRead(mech, addr, out, n) == (ssize_t)n;
}

bool SliceReadable(uintptr_t addr, size_t n)
{
    if (n == 0) return true;
    if (addr < 0x1000 || addr + n < addr) return false;
    const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    const uintptr_t last = addr + n - 1;
    char b;
    if (!SliceRead(addr, &b, 1)) return false;
    for (uintptr_t pg = (addr & ~(page - 1)) + page; pg <= last && pg > addr; pg += page)
        if (!SliceRead(pg, &b, 1)) return false;
    return true;
}

static const unsigned char kSelfTestBytes[32] = {
    0x54, 0x50, 0x46, 0x32, 0x4d, 0x50, 0x20, 0x73, 0x6c, 0x69, 0x63, 0x65, 0x20, 0x72, 0x65, 0x61,
    0x64, 0x20, 0x73, 0x65, 0x6c, 0x66, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x20, 0x21, 0x7e, 0x00, 0xff,
};

bool SliceReadSelfTest(int mech, char* why, size_t cap)
{
    if (cap) why[0] = 0;
    const long page = sysconf(_SC_PAGESIZE);
    void* m = mmap(nullptr, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { snprintf(why, cap, "mmap failed (%s)", strerror(errno)); return false; }
    const uintptr_t base = (uintptr_t)m;
    for (long i = 0; i < page; i++) ((unsigned char*)m)[i] = (unsigned char)(i * 7 + 3);
    bool ok = mprotect((void*)(base + (uintptr_t)page), (size_t)page, PROT_NONE) == 0;
    if (!ok) snprintf(why, cap, "mprotect failed (%s)", strerror(errno));

    unsigned char buf[64];
    ssize_t r;
    if (ok) {   // a readable heap range, contents intact
        r = RawRead(mech, base + 100, buf, 64);
        ok = r == 64 && memcmp(buf, (char*)m + 100, 64) == 0;
        if (!ok) snprintf(why, cap, "readable page: got %zd", r);
    }
    if (ok) {   // our own .rodata
        r = RawRead(mech, (uintptr_t)kSelfTestBytes, buf, sizeof(kSelfTestBytes));
        ok = r == (ssize_t)sizeof(kSelfTestBytes) && memcmp(buf, kSelfTestBytes, sizeof(kSelfTestBytes)) == 0;
        if (!ok) snprintf(why, cap, ".rodata: got %zd", r);
    }
    if (ok) {   // address 0
        r = RawRead(mech, 0, buf, 8);
        ok = r < 0;
        if (!ok) snprintf(why, cap, "address 0 read %zd bytes", r);
    }
    if (ok) {   // a low page below vm.mmap_min_addr: never mapped
        r = RawRead(mech, 0x1000, buf, 8);
        ok = r < 0;
        if (!ok) snprintf(why, cap, "address 0x1000 read %zd bytes", r);
    }
    if (ok) {   // a PROT_NONE page
        r = RawRead(mech, base + (uintptr_t)page, buf, 16);
        ok = r < 0;
        if (!ok) snprintf(why, cap, "PROT_NONE page read %zd bytes", r);
    }
    if (ok) {   // a read straddling into it must not succeed; what it did read must be right
        r = RawRead(mech, base + (uintptr_t)page - 16, buf, 32);
        ok = r != 32 && (r <= 0 || (r <= 16 && memcmp(buf, (char*)m + page - 16, (size_t)r) == 0));
        if (ok && mech == kSliceReadVm) ok = r == 16;   // C-READ-1: a partial count
        if (!ok) snprintf(why, cap, "straddling read got %zd", r);
    }
    munmap(m, (size_t)page * 2);
    return ok;
}

bool SliceReadInit()
{
    char why[160];
    if (SliceReadSelfTest(kSliceReadVm, why, sizeof(why))) {
        SliceReadSelect(kSliceReadVm);
        SliceLog("[slice] guarded reads: process_vm_readv on our own pid (self-test passed)\n");
        return true;
    }
    SliceLog("[slice] guarded reads: process_vm_readv self-test FAILED (%s)\n", why);
    if (SliceReadSelfTest(kSliceReadPipe, why, sizeof(why))) {
        SliceReadSelect(kSliceReadPipe);
        SliceLog("[slice] guarded reads: pipe probe (self-test passed)\n");
        return true;
    }
    SliceLog("[slice] guarded reads: pipe probe self-test FAILED (%s) -- every guarded read fails, "
             "the cancel point stays off and decoders that read game memory get nothing\n", why);
    SliceReadSelect(kSliceReadNone);
    return false;
}

// ---- libstdc++ containers --------------------------------------------------------------------

bool SliceReadStdString(uintptr_t obj, char* out, size_t cap, size_t* lenOut, size_t maxLen)
{
    if (lenOut) *lenOut = 0;
    if (cap) out[0] = 0;
    uint64_t h[3];   // p, len, local buffer / heap capacity
    if (!SliceRead(obj, h, sizeof(h))) return false;
    const uintptr_t p = (uintptr_t)h[0];
    const size_t len = (size_t)h[1];
    if (len > maxLen || len >= cap) return false;
    if (p == obj + 16) {
        if (len > 15) return false;
    } else if (!p || h[2] < len) {
        return false;
    }
    if (!SliceRead(p, out, len + 1) || out[len] != 0) { out[0] = 0; return false; }
    if (lenOut) *lenOut = len;
    return true;
}

bool SliceReadStdVector(uintptr_t obj, size_t stride, size_t maxCount, SliceVec* out)
{
    out->begin = 0;
    out->count = 0;
    if (!stride) return false;
    uint64_t v[3];
    if (!SliceRead(obj, v, sizeof(v))) return false;
    const uintptr_t b = (uintptr_t)v[0], e = (uintptr_t)v[1], c = (uintptr_t)v[2];
    if (!b) return e == 0 && c == 0;
    if (b > e || e > c) return false;
    if ((e - b) % stride || (c - b) % stride) return false;
    const size_t count = (e - b) / stride;
    if (count > maxCount) return false;
    if (count && !SliceReadable(b, e - b)) return false;
    out->begin = b;
    out->count = count;
    return true;
}

bool SliceWalkStdMap(uintptr_t obj, size_t maxNodes, SliceRbVisit visit, void* ctx, size_t* visited)
{
    if (visited) *visited = 0;
    const uintptr_t header = obj + 8;
    uint64_t h[5];   // header: color, parent (root), left (leftmost), right (rightmost); node_count
    if (!SliceRead(header, h, sizeof(h))) return false;
    const uintptr_t root = (uintptr_t)h[1], leftmost = (uintptr_t)h[2], rightmost = (uintptr_t)h[3];
    const size_t count = (size_t)h[4];
    if (count > maxNodes) return false;
    if (count == 0) return root == 0 && leftmost == header && rightmost == header;
    uintptr_t rootParent = 0;
    if (!root || !SliceReadT(root + 8, &rootParent) || rootParent != header) return false;

    size_t seen = 0;
    uintptr_t x = leftmost, last = 0;
    while (x != header) {
        if (seen >= count) return false;              // longer than node_count: a cycle or a torn tree
        uint64_t n[4];                                 // color, parent, left, right
        if (!SliceRead(x, n, sizeof(n))) return false;
        seen++;
        if (visited) *visited = seen;
        last = x;
        if (visit && !visit(x, x + 0x20, ctx)) return true;
        // libstdc++ _Rb_tree_increment
        if (n[3]) {
            x = (uintptr_t)n[3];
            for (size_t guard = 0;; guard++) {
                uintptr_t l;
                if (guard > count || !SliceReadT(x + 0x10, &l)) return false;
                if (!l) break;
                x = l;
            }
        } else {
            uintptr_t y = (uintptr_t)n[1];
            for (size_t guard = 0;; guard++) {
                uintptr_t yr;
                if (guard > count + 1 || !SliceReadT(y + 0x18, &yr)) return false;
                if (x != yr) break;
                x = y;
                if (!SliceReadT(y + 8, &y)) return false;
            }
            uintptr_t xr;
            if (!SliceReadT(x + 0x18, &xr)) return false;
            if (xr != y) x = y;
        }
    }
    return seen == count && last == rightmost;
}

bool SliceStdFunctionParts(uintptr_t fn, uintptr_t* manager, uintptr_t* invoker)
{
    uint64_t p[2];
    if (!SliceRead(fn + 0x10, p, sizeof(p))) return false;
    *manager = (uintptr_t)p[0];
    *invoker = (uintptr_t)p[1];
    return true;
}

// ---- factories and callers ----------------------------------------------------------------------

#define P14A 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55
#define P14B 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54
#define P18  0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x54, 0x53, 0x4c, 0x8d, 0xa5, 0x90, 0xf2, 0xff, 0xff

// Appendix B of SLICE_CORE.md, by tag. Bytes and steals re-checked against the
// binary (capstone boundary, no RIP-relative operand or branch); stackArgs from the
// C-FAC-4 sweep: SaveGame reads [rbp+0x10] (0x15ed176), Book [rbp+0x10..0x24]
// (0x15ecdbc-0x15ecde5); the other 35 touch no stack argument.
static const SliceFactoryInfo kFactories[] = {
    { 0x15eb600, "SetGameSpeed",                     0x00, 15, 18, false, { P18 } },
    { 0x15eb690, "SetCalendarSpeed",                 0x01, 17, 18, false, { P18 } },
    { 0x15ebc00, "UpdateLogo",                       0x02, -1, 14, false, { P14A } },
    { 0x15efda0, "CreateLine",                       0x03,  7, 14, false, { P14A } },
    { 0x15ebd00, "DeleteLine",                       0x04,  9, 14, false, { P14B } },
    { 0x15f0050, "UpdateLine",                       0x05,  8, 14, false, { P14A } },
    { 0x15ed550, "SetLine",                          0x06,  6, 14, false, { P14A } },
    { 0x15ebe00, "Reverse",                          0x07, 10, 14, false, { P14B } },
    { 0x15ebf00, "SetUserStopped",                   0x08, -1, 14, false, { P14A } },
    { 0x15ec000, "SetVehicleTargetMaintenanceState", 0x09, 12, 14, false, { P14B } },
    { 0x15ec150, "SetVehicleShouldDepart",           0x0a, -1, 15, false,
      { 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x49, 0x89, 0xf6, 0x41, 0x55 } },
    { 0x15ec220, "SendToDepot",                      0x0b,  5, 14, false, { P14A } },
    { 0x15ecf00, "SellVehicle",                      0x0c,  3, 14, false, { P14B } },
    { 0x15ef3b0, "BuyVehicle",                       0x0d,  2, 14, false, { P14A } },
    { 0x15ef8c0, "ReplaceVehicle",                   0x0e,  4, 14, false, { P14A } },
    { 0x15ee930, "BuildProposal",                    0x0f,  0, 14, false, { P14A } },
    { 0x15ec320, "RemoveField",                      0x10, -1, 14, false, { P14B } },
    { 0x15ee340, "tag_0x11",                         0x11, -1, 14, false, { P14A } },
    { 0x15ec420, "RemoveTown",                       0x12, -1, 14, false, { P14B } },
    { 0x15eb720, "tag_0x13",                         0x13, -1, 18, false, { P18 } },
    { 0x15ec520, "tag_0x14",                         0x14, -1, 14, false, { P14A } },
    { 0x15ed6c0, "tag_0x15",                         0x15, -1, 15, false,
      { 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x89, 0xd7, 0x41, 0x56 } },
    { 0x15ec610, "ConnectTownsAndIndustries",        0x16, -1, 15, false,
      { 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x45, 0x89, 0xc7, 0x41, 0x56 } },
    { 0x15ec940, "SetSimBuildingManualDevelopment",  0x17, -1, 14, false, { P14A } },
    { 0x15eca40, "SetSimBuildingClosureTimeStamp",   0x18, -1, 14, false, { P14A } },
    { 0x15ed8b0, "tag_0x19",                         0x19, -1, 16, false,
      { 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x49, 0x89, 0xf7, 0x48, 0x89, 0xd6 } },
    { 0x15eb7b0, "SetDate",                          0x1a, 16, 18, false, { P18 } },
    { 0x15ed140, "SaveGame",                         0x1b, -1, 15, true,
      { 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x49, 0x89, 0xcf, 0x41, 0x56 } },
    { 0x15ecb40, "SetColor",                         0x1c, 13, 14, false, { P14A } },
    { 0x15ee6d0, "SetName",                          0x1d, 14, 14, false, { P14A } },
    { 0x15ecc80, "SetVehicleManualDeparture",        0x1e, -1, 14, false, { P14A } },
    { 0x15ecd80, "Book",                             0x1f, -1, 14, true,  { P14A } },
    { 0x15ee130, "SendScriptEvent",                  0x20, -1, 15, false,
      { 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x49, 0x89, 0xd7, 0x41, 0x56 } },
    { 0x15eb840, "tag_0x21",                         0x21, -1, 18, false, { P18 } },
    { 0x15eb8d0, "tag_0x22",                         0x22, -1, 18, false, { P18 } },
    { 0x15eb980, "tag_0x23",                         0x23, -1, 18, false, { P18 } },
    { 0x15eba20, "tag_0x24",                         0x24, -1, 18, false, { P18 } },
};
#undef P14A
#undef P14B
#undef P18
static const size_t kFactoryCount = sizeof(kFactories) / sizeof(kFactories[0]);

const SliceFactoryInfo* SliceFactories(size_t* count)
{
    if (count) *count = kFactoryCount;
    return kFactories;
}

const SliceFactoryInfo* SliceFactoryByRva(uintptr_t rva)
{
    for (size_t i = 0; i < kFactoryCount; i++) if (kFactories[i].rva == rva) return &kFactories[i];
    return nullptr;
}

const SliceFactoryInfo* SliceFactoryByTag(int tag)
{
    for (size_t i = 0; i < kFactoryCount; i++) if (kFactories[i].tag == tag) return &kFactories[i];
    return nullptr;
}

// C-SCRIPT-1: the factory return addresses in scripting::SetupCommandInterface's
// makers and in the legacy scripting::AddFunction (0x1d8737a). Sorted.
static const uintptr_t kScriptReturns[] = {
    0x1950f23, 0x1950fc1, 0x1951063, 0x1951b68, 0x1969681, 0x1969ab1, 0x1969f2c, 0x196a371,
    0x196a7a1, 0x196abd1, 0x196b033, 0x196b43c, 0x196b929, 0x196bd64, 0x196c234, 0x196c704,
    0x196cbd4, 0x196d43f, 0x196dad8, 0x196e056, 0x196e662, 0x196f04c, 0x196f9d3, 0x197023e,
    0x1971333, 0x1972ad2, 0x1973034, 0x1973c70, 0x1974841, 0x1974ffc, 0x1975884, 0x19763ff,
    0x1977628, 0x1d8737a,
};
static const size_t kScriptReturnCount = sizeof(kScriptReturns) / sizeof(kScriptReturns[0]);
// C-SCRIPT-2: the two ranges holding them, used only as a consistency check.
static const SliceExecRange kScriptRanges[] = { { 0x1950ef0, 0x1978700 }, { 0x1d86ff0, 0x1d878da } };

bool SliceIsScriptCaller(uintptr_t retRva)
{
    size_t lo = 0, hi = kScriptReturnCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kScriptReturns[mid] < retRva) lo = mid + 1;
        else hi = mid;
    }
    return lo < kScriptReturnCount && kScriptReturns[lo] == retRva;
}

// C-PTR-5: the factory calls whose Command reaches CommandList::Add with no other
// call in between. Generated by gen_armable.py from build 35924 (SLICE_CORE.md 3.5):
// {the factory call's return address, the factory, the end of the analysed window
// [ret - 5, end), the helper the window returns through (0: the window ends with the
// E8 call to Add), FNV-1a 32 of the window bytes}. In each direct window a linear
// sweep found no control-flow instruction between the two calls. Through line_util
// 0x2eb0fa0 every path from the factory's return reaches the helper's ret calling
// only operator delete (PLT 0x6dbcd0) and 0xaa43f0 (which calls only operator delete),
// or ends in the noreturn __stack_chk_fail (0x2eb1d56); its only callers are the two
// below, each calling Add next. .text carries no relocation, so the analysed file
// bytes are the live bytes.
struct ArmableSite { uintptr_t ret, factory, end, helper; uint32_t fnv; };
static const ArmableSite kArmableSites[] = {
    { 0xdd4e99, 0x15ee930, 0xdd4ec3, 0, 0x4b21ac4c },
    { 0xe0b164, 0x15eb600, 0xe0b18a, 0, 0x2f697dec },
    { 0xe13be4, 0x15eb600, 0xe13c07, 0, 0x7652c67d },
    { 0xe13d78, 0x15eb600, 0xe13d9b, 0, 0x5a7e07e5 },
    { 0xe14bec, 0x15eb600, 0xe14c0f, 0, 0xb31980df },
    { 0xe34860, 0x15ee930, 0xe34895, 0, 0x5ac2c5ba },
    { 0xe4f6bd, 0x15ee930, 0xe4f6f2, 0, 0xb04fa416 },
    { 0xe59622, 0x15ee930, 0xe59657, 0, 0x3d44345b },
    { 0xe86452, 0x15ee930, 0xe86487, 0, 0x6209f1d9 },
    { 0xeaaf79, 0x15ee930, 0xeaafb2, 0, 0x2ba7d4d2 },
    { 0xec0551, 0x15ee340, 0xec057b, 0, 0xac076151 },
    { 0xed6da9, 0x15ee930, 0xed6ddd, 0, 0xf8887081 },
    { 0xf01832, 0x15ec420, 0xf01855, 0, 0x13582945 },
    { 0xf229a5, 0x15ee930, 0xf229d7, 0, 0x505ae8b9 },
    { 0xf6b88e, 0x15eb600, 0xf6b8b1, 0, 0x246edb03 },
    { 0xf6bce5, 0x15eb7b0, 0xf6bd08, 0, 0xcf181788 },
    { 0xf6c28d, 0x15eb600, 0xf6c2b0, 0, 0xdbb90407 },
    { 0xf70410, 0x15eb690, 0xf7042f, 0, 0x0dbfb1a1 },
    { 0xfcf497, 0x15ecd80, 0xfcf4c8, 0, 0x0f773227 },
    { 0xfcf5b6, 0x15ecd80, 0xfcf5d2, 0, 0x87e3f3a0 },
    { 0xfcf77a, 0x15ecd80, 0xfcf7ab, 0, 0x646818b8 },
    { 0xfcf89e, 0x15ecd80, 0xfcf8ba, 0, 0x60a17c7e },
    { 0xfe794c, 0x15eb600, 0xfe796f, 0, 0x8c45f73f },
    { 0xfe7ae7, 0x15ecd80, 0xfe7b0e, 0, 0x61351ecd },
    { 0xfeb9fe, 0x15ed140, 0xfeba31, 0, 0x1e37bf97 },
    { 0x102f876, 0x15ec420, 0x102f892, 0, 0x58525f79 },
    { 0x102fbd2, 0x15ee340, 0x102fbf9, 0, 0xa4458eae },
    { 0x1030a4c, 0x15ed8b0, 0x1030a76, 0, 0x98e6e59d },
    { 0x1073fd8, 0x15ec610, 0x1073ffb, 0, 0x975c97cc },
    { 0x10a95b9, 0x15ed140, 0x10a95ec, 0, 0x9e5a2501 },
    { 0x10bec4e, 0x15ecb40, 0x10bec71, 0, 0x22ab8af8 },
    { 0x10bf829, 0x15f0050, 0x10bf850, 0, 0x26c2fdec },
    { 0x10c0679, 0x15f0050, 0x10c06a0, 0, 0x064c63ec },
    { 0x10c1770, 0x15f0050, 0x10c1796, 0, 0x98661285 },
    { 0x10c1946, 0x15f0050, 0x10c196c, 0, 0x019b1c93 },
    { 0x10c25fa, 0x15f0050, 0x10c2624, 0, 0x201bab85 },
    { 0x10c305c, 0x15f0050, 0x10c3083, 0, 0x539b4c0c },
    { 0x10c9e1c, 0x15f0050, 0x10c9e43, 0, 0x942dcd5b },
    { 0x10ca6e0, 0x15f0050, 0x10ca703, 0, 0x898df7bb },
    { 0x110dbd7, 0x15ebc00, 0x110dbfe, 0, 0xeea888eb },
    { 0x112243b, 0x15eb600, 0x112245e, 0, 0x409cc243 },
    { 0x112c0ef, 0x15eb600, 0x112c119, 0, 0x0da11ae1 },
    { 0x11d1569, 0x15ecd80, 0x11d1594, 0, 0x69778de8 },
    { 0x1207337, 0x15f0050, 0x1207361, 0, 0x8d1503e6 },
    { 0x12193d0, 0x15f0050, 0x12193fa, 0, 0xb8e5826c },
    { 0x122e106, 0x15ec520, 0x122e129, 0, 0x1e6ff9aa },
    { 0x122e35b, 0x15ed6c0, 0x122e381, 0, 0xf12b04b4 },
    { 0x122e9a8, 0x15ed6c0, 0x122e9ce, 0, 0x464edd2c },
    { 0x1233813, 0x15ed6c0, 0x1233836, 0, 0xaeecff5f },
    { 0x1233d33, 0x15ed6c0, 0x1233d56, 0, 0xf626935a },
    { 0x12409e2, 0x15ee930, 0x1240a17, 0, 0x0c740430 },
    { 0x1240e99, 0x15ee930, 0x1240ece, 0, 0x1d2182c8 },
    { 0x1241402, 0x15ee930, 0x1241437, 0, 0x2774cf6f },
    { 0x12469b1, 0x15ecb40, 0x12469d4, 0, 0x93a9c488 },
    { 0x126f37b, 0x15ec000, 0x126f3a3, 0, 0x22c2cf87 },
    { 0x126f62b, 0x15ecb40, 0x126f653, 0, 0x0689157c },
    { 0x1274234, 0x15ec220, 0x127425b, 0, 0x84432b07 },
    { 0x127bd06, 0x15ef8c0, 0x127bd29, 0, 0x20857885 },
    { 0x127c000, 0x15ef3b0, 0x127c023, 0, 0x108474f4 },
    { 0x127c219, 0x15ecf00, 0x127c23c, 0, 0xbb25c965 },
    { 0x127d0d0, 0x15ecf00, 0x127d0f3, 0, 0x771e5f3f },
    { 0x12cb3d7, 0x15eb600, 0x12cb3fa, 0, 0x951bdf08 },
    { 0x1326aa0, 0x15ebd00, 0x1326ac3, 0, 0x68749762 },
    { 0x1327526, 0x15ee6d0, 0x132754d, 0, 0x19c1052c },
    { 0x132bbdc, 0x15f0050, 0x132bc0a, 0, 0x70a69d1a },
    { 0x132bec4, 0x15f0050, 0x132bef2, 0, 0xe93c6682 },
    { 0x132c513, 0x15f0050, 0x132c53e, 0, 0xd504cc9c },
    { 0x1343e6b, 0x15eb840, 0x1343e8a, 0, 0x6ea59bd3 },
    { 0x14287df, 0x15ee6d0, 0x1428806, 0, 0x06e9af5d },
    { 0x142fed9, 0x15ec220, 0x142fefc, 0, 0x52148ebd },
    { 0x1431102, 0x15ebf00, 0x1431129, 0, 0xd80de86f },
    { 0x1431204, 0x15ebf00, 0x143122b, 0, 0x27d7f59d },
    { 0x1432b39, 0x15ecf00, 0x1432b5f, 0, 0xbaf89107 },
    { 0x14334f7, 0x15ed550, 0x1433521, 0, 0xe4176c4f },
    { 0x1437ec1, 0x15ed550, 0x1437ee8, 0, 0x329319dd },
    { 0x14385fa, 0x15ed550, 0x1438633, 0, 0x5c782836 },
    { 0x144407a, 0x15ecb40, 0x144409d, 0, 0x33fc7264 },
    { 0x144418a, 0x15ecb40, 0x14441ad, 0, 0xccd6f590 },
    { 0x144429a, 0x15ecb40, 0x14442bd, 0, 0x2bfa4064 },
    { 0x144439b, 0x15ebe00, 0x14443be, 0, 0x5f82c1e1 },
    { 0x14465db, 0x15ed550, 0x14465fe, 0, 0x423ebca6 },
    { 0x145e1ac, 0x15ee930, 0x145e1e1, 0, 0x5d268e18 },
    { 0x145e545, 0x15ee930, 0x145e577, 0, 0x362e0eeb },
    { 0x145ef82, 0x15ee930, 0x145efb7, 0, 0x1f2fce79 },
    { 0x145fbc5, 0x15ee930, 0x145fbf3, 0, 0xfc8f0704 },
    { 0x1789a5a, 0x15eb8d0, 0x1789a7d, 0, 0xc3a74fc8 },
    { 0x2eb1c49, 0x15efda0, 0x2eb1d5b, 0x2eb0fa0, 0x3a45f34f },
    { 0x2fbb2e2, 0x15ef8c0, 0x2fbb309, 0, 0xd44b8cae },
};
static const size_t kArmableCount = sizeof(kArmableSites) / sizeof(kArmableSites[0]);
// A helper's callers, each window [call, end) ending with the E8 call to Add.
struct HelperCaller { uintptr_t helper, call, end; uint32_t fnv; };
static const HelperCaller kHelperCallers[] = {
    { 0x2eb0fa0, 0x10d4c08, 0x10d4c34, 0x2f1eaf95 },
    { 0x2eb0fa0, 0x10d9d9c, 0x10d9dc4, 0x06b7d339 },
};
static const size_t kHelperCallerCount = sizeof(kHelperCallers) / sizeof(kHelperCallers[0]);
static std::atomic<bool> g_armableOk{false};   // the set matched the image (SliceCoreStaticChecks)

bool SliceIsArmableSite(uintptr_t retRva, uintptr_t factoryRva)
{
    if (!g_armableOk.load(std::memory_order_relaxed)) return false;
    size_t lo = 0, hi = kArmableCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kArmableSites[mid].ret < retRva) lo = mid + 1;
        else hi = mid;
    }
    return lo < kArmableCount && kArmableSites[lo].ret == retRva && kArmableSites[lo].factory == factoryRva;
}

// The complete verified factory-to-Add windows are the default player barrier.
// These exclusions are concrete engine/setup/file operations, not decoder gaps.
bool SlicePlayerAddSite(uintptr_t rva)
{
    switch (rva) {
        case 0x2fbb309: // deterministic missing-resource vehicle repair
        case 0x1789a7d: // ecs animal simulation
        case 0x1343e8a: // FinalizeGuiConfiguration no-cost setup
        case 0xfe796f: case 0x112245e: case 0x112c119: // game/menu speed bookkeeping
        case 0xfe7b0e: // game step accounting
        case 0xfeba31: case 0x10a95ec: // autosave/manual save
        case 0x110dbfe: // load-game preview logo
            return false;
        default: break;
    }
    for (const auto& site : kArmableSites)
        if (!site.helper && site.end == rva) return true;
    for (const auto& site : kHelperCallers)
        if (site.end == rva) return true;
    return false;
}

// A `done` invoker is called only inside the game's .text (env.code), not merely in
// an executable segment: build 35924's first R+X segment also maps .rodata and
// .eh_frame.
bool SliceInGameCode(uintptr_t addr)
{
    for (int i = 0; i < g_env.nCode; i++)
        if (addr >= g_env.code[i].lo && addr < g_env.code[i].hi) return true;
    return false;
}

const char* SliceAddCallerKind(uintptr_t addRetRva)
{
    if (addRetRva == 0xa2f5c2 || addRetRva == 0x11225a9) return "script-sink";
    if (addRetRva == 0x1d873b2) return "legacy-script";
    return "ui";
}

int SliceCommandTag(uintptr_t cmd)
{
    uintptr_t data = 0;
    uint8_t tag = 0;
    if (!SliceReadT(cmd, &data) || !data || !SliceReadT(data + 0xd48, &tag)) return -1;
    return tag;
}

static uint32_t Fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

// [lo, hi) of the image lies inside one executable segment.
static bool InOneExec(uintptr_t loRva, uintptr_t hiRva)
{
    const uintptr_t lo = g_env.base + loRva, hi = g_env.base + hiRva;
    for (int i = 0; i < g_env.nExec; i++)
        if (hi > lo && lo >= g_env.exec[i].lo && hi <= g_env.exec[i].hi) return true;
    return false;
}

// The target RVA of the E8 call at siteRva, or UINTPTR_MAX when there is none.
static uintptr_t CallTargetRva(uintptr_t siteRva)
{
    if (!InOneExec(siteRva, siteRva + 5)) return UINTPTR_MAX;
    const uint8_t* c = (const uint8_t*)(g_env.base + siteRva);
    if (c[0] != 0xE8) return UINTPTR_MAX;
    int32_t rel;
    memcpy(&rel, c + 1, 4);
    return siteRva + 5 + (intptr_t)rel;
}

static bool WindowMatches(uintptr_t loRva, uintptr_t hiRva, uint32_t fnv)
{
    return InOneExec(loRva, hiRva) && Fnv1a((const uint8_t*)(g_env.base + loRva), hiRva - loRva) == fnv;
}

bool SliceCoreStaticChecks()
{
    bool ok = true;
    uint64_t tagsSeen = 0;
    for (size_t i = 0; i < kFactoryCount; i++) {
        const SliceFactoryInfo& f = kFactories[i];
        const bool shape = f.tag <= 0x24 && !(tagsSeen & (1ull << f.tag)) && f.steal >= 14 && f.steal <= 18 &&
                           PrologueSteal(f.prologue, 14) == f.steal && SliceFactoryByRva(f.rva) == &f;
        if (!shape) {
            SliceLog("[slice] CHECK FAILED: factory table entry %s (%lx)\n", f.name, (unsigned long)f.rva);
            ok = false;
        }
        if (f.tag <= 0x24) tagsSeen |= 1ull << f.tag;
    }
    if (kFactoryCount != 37) { SliceLog("[slice] CHECK FAILED: %zu factories, expected 37\n", kFactoryCount); ok = false; }
    for (size_t i = 0; i < kScriptReturnCount; i++) {
        const uintptr_t r = kScriptReturns[i];
        const bool inRange = (r >= kScriptRanges[0].lo && r < kScriptRanges[0].hi) ||
                             (r >= kScriptRanges[1].lo && r < kScriptRanges[1].hi);
        if ((i && kScriptReturns[i - 1] >= r) || !inRange) {
            SliceLog("[slice] CHECK FAILED: script return address %lx out of order or range\n", (unsigned long)r);
            ok = false;
        }
    }
    // The armable set: sorted, a table factory, never a script return, a bounded
    // window, and every helper with its callers.
    size_t viaHelper = 0;
    for (size_t i = 0; i < kArmableCount; i++) {
        const ArmableSite& a = kArmableSites[i];
        bool good = (!i || kArmableSites[i - 1].ret < a.ret) && SliceFactoryByRva(a.factory) &&
                    !SliceIsScriptCaller(a.ret) && a.ret > 5 && a.end > a.ret && a.end - (a.ret - 5) <= 0x400;
        if (good && a.helper) {
            viaHelper++;
            size_t callers = 0;
            for (size_t k = 0; k < kHelperCallerCount; k++) {
                const HelperCaller& h = kHelperCallers[k];
                if (h.helper != a.helper) continue;
                callers++;
                good = good && h.end > h.call + 5 && h.end - h.call <= 0x400;
            }
            good = good && callers > 0;
        }
        if (!good) {
            SliceLog("[slice] CHECK FAILED: armable call site table entry %lx\n", (unsigned long)a.ret);
            ok = false;
        }
    }
    if (kArmableCount != 88 || viaHelper != 1 || kHelperCallerCount != 2) {
        SliceLog("[slice] CHECK FAILED: %zu armable call sites (%zu through a helper, %zu helper callers), expected 88 (1, 2)\n",
                 kArmableCount, viaHelper, kHelperCallerCount);
        ok = false;
    }

    if (ok && g_env.base) {
        // Each script return address follows an E8 call to a factory.
        for (size_t i = 0; i < kScriptReturnCount; i++) {
            if (!SliceFactoryByRva(CallTargetRva(kScriptReturns[i] - 5))) {
                SliceLog("[slice] CHECK FAILED: no factory call before script return address %lx\n",
                         (unsigned long)kScriptReturns[i]);
                ok = false;
            }
        }
        // Each armable window: the E8 call to its factory, the analysed bytes, and the
        // E8 call to Add at its end -- or, through a helper, at the end of each caller's.
        for (size_t i = 0; i < kArmableCount; i++) {
            const ArmableSite& a = kArmableSites[i];
            const bool good = CallTargetRva(a.ret - 5) == a.factory && WindowMatches(a.ret - 5, a.end, a.fnv) &&
                              (a.helper || CallTargetRva(a.end - 5) == kSliceRvaAdd);
            if (!good) {
                SliceLog("[slice] CHECK FAILED: armable call site %lx (factory %lx) does not match the image\n",
                         (unsigned long)a.ret, (unsigned long)a.factory);
                ok = false;
            }
        }
        for (size_t k = 0; k < kHelperCallerCount; k++) {
            const HelperCaller& h = kHelperCallers[k];
            if (CallTargetRva(h.call) != h.helper || CallTargetRva(h.end - 5) != kSliceRvaAdd ||
                !WindowMatches(h.call, h.end, h.fnv)) {
                SliceLog("[slice] CHECK FAILED: helper caller %lx of %lx does not match the image\n",
                         (unsigned long)h.call, (unsigned long)h.helper);
                ok = false;
            }
        }
        if (ok)
            SliceLog("[slice] armable call sites: %zu verified against the image (%zu through a helper, %zu helper "
                     "callers)\n", kArmableCount, viaHelper, kHelperCallerCount);
        else
            SliceLog("[slice] the caller tables do not match this image -- nothing is patched\n");
    }
    g_armableOk.store(ok && g_env.base != 0);
    return ok;
}
