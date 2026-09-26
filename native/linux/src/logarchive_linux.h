// logarchive_linux.h -- the Linux counterpart of logarchive.h: every log a bug
// report needs, gathered in one folder:
//
//   $XDG_DATA_HOME/tpf2mp/logs/<yyyymmdd-hhmmss>-previous/   the last run, saved at the next start
//   $XDG_DATA_HOME/tpf2mp/logs/<yyyymmdd-hhmmss>-now/        a copy taken while the game runs
//
// ($HOME/.local/share when XDG_DATA_HOME is unset, as in datadir_linux.h; under
// Snap Steam both name ~/snap/steam/common/.local/share.)
//
// Why at start: a restart destroys the evidence. The game truncates its own log
// (stdout.txt) on launch, the bridge and menu recreate theirs, and the loader
// and the Lua script append theirs to the last run's. The loader (boot.cpp)
// runs this from its constructor, before main() and before it dlopens any other
// library of ours, so the previous run's files are still intact.
//
// The game's files on Linux (build 35924), all in
// <Steam>/userdata/<account>/1066780/local/crash_dump/:
//   stdout.txt      the game's log with the Lua script's print lines.
//   stdout_old.txt  a copy of stdout.txt the game takes at start: the run before.
//   lockfile        the running game's PID; still there when a run ended without
//                   the game's own cleanup.
//   <id>.dmp        Breakpad minidumps. Unlike Windows there is no per-dump
//                   <id>_stdout_old.txt: the binary has no "_stdout_old" string.
// Evidence (Linux RVAs):
//  - main 0x95e6c0 calls 0x9b3d50 at 0x95e8e3 (_start 0x97a270 hands main to
//    __libc_start_main: 0x97a291 lea rdi -> 0x95e6c0, 0x97a298 call [0x5a462e8],
//    the GLOB_DAT slot of __libc_start_main). 0x9b3d50 joins "crash_dump/" onto
//    the user folder (0x9b4ac1 lea, 0x9b4ad2 call 0x9bfab0) and calls 0x9b0b10
//    with it at 0x9b4b22. No .init_array entry (785 relocated entries) reaches
//    it: a reverse graph over every direct call/jmp rel32 and RIP-relative lea
//    finds only main and _start above 0x9b3d50, and there is no
//    DT_PREINIT_ARRAY. The ELF entry 0x5bc11a0 is a stub in an extra R+X PT_LOAD
//    at 0x5bc1000, outside .text: it saves the registers, calls 0x5bc1250 and
//    returns to the address that call gives back. ld.so runs the preloaded
//    loader's constructor before it jumps to the entry, so this code always
//    runs before the game's.
//  - 0x9b0b10 joins "stdout.txt" (0x9b0b14), "stdout_old.txt" (0x9b0b5d) and
//    "lockfile" (0x9b0b7d). If lockfile exists (0x9b0bc4 -> 0x31f5090 ->
//    boost::filesystem::detail::status) it appends "__CRASHDB_CRASH__ Unexpected
//    program termination" to stdout.txt (0x9b0d5e ofstream(path, app)) and
//    removes lockfile (0x9b0f0f -> 0x31f6c00 -> detail::remove); it also names a
//    lockfile whose process is alive (0x9b12d0 lea "... Process seems to be still
//    running"). It writes getpid() into lockfile (0x9b1025 filebuf::open(out),
//    0x9b10a5 -> 0x322d460 jmp getpid) and copies stdout.txt over stdout_old.txt
//    (0x9b1283 call 0x31f69b0(stdout.txt, stdout_old.txt, 1); for a nonzero flag,
//    0x31f69ee test dl / 0x31f69fc je, that calls detail::copy_file(from, to,
//    unsigned options, error_code*) at 0x31f6a33 with options 2 (0x31f6a28):
//    copy_options::overwrite_existing).
//  - 0x9b3d50 only then joins "stdout.txt" again (0x9b4c90), opens it (0x9b4cd7
//    call 0x9b7a50) and points the standard streams at it (0x9b4d0a, 0x9b4d2b
//    basic_ios::rdbuf). That open truncates: 0x9b7a50 builds an ofstream
//    (0x9b7a88 ios_base::ios_base, 0x9b7b29 basic_filebuf ctor) and calls
//    basic_filebuf::open(const char*, openmode) at 0x9b7b4c (PLT 0x6dc020) with
//    edx = 0x10 (0x9b7b44): ios_base::out alone, which libstdc++ opens with
//    fopen mode "w". On this machine stdout.txt keeps its inode (birth 17:32)
//    but holds only the last run, and stdout_old.txt, written 17 s before it at
//    the last start, differs from it.
//  - 0x9afb90 removes lockfile (0x9afbdf lea, 0x9afc6d -> 0x31f6c00); it is
//    called from Run 0x6e0aad (0x6e11df), from the "Minidump Callback" function
//    0x9b1450 (0x9b1633) and from 0x9b3d50 (0x9b57e8).
//  - Run2 0x9b1a90 joins "crash_dump/" at 0x9b1c62 into [rbp-0xf80]; after an
//    unclean end (flag 0x5a4f64c, set at 0x9b0bff) it reads stdout_old.txt line
//    by line (0x9b2e47, 0x9b3071 filebuf::open, 0x9b312e getline) for
//    "__CRASHDB_DUMP__ " (0x9b3866) and joins "<id>.dmp" (0x9b364f) onto that
//    folder (0x9b368a, 0x9b36dc path::operator/=).
//
// State/config and lobby streams follow the logs (8 MiB tails, always copied).
// Lobby invitation/password fields are masked in copies. about.txt records the
// installed version, kernel, time zone and module ELF identities.
//
// What goes in: the data folder's *.log (MOVED at start, since the next run
// writes fresh ones), the lobby folder's *.log ($XDG_DATA_HOME/tpf2mp/netpunch,
// and <game>/netpunch, the Lua side's fallback), the game's stdout.txt, and the
// crash dumps written since the last archive. stdout_old.txt is left out: at
// start it holds the run before the last one, which that run's start already
// saved. The newest TPF2_LOG_KEEP folders of each kind are kept. about.txt
// names the files without the user's paths: these folders get sent in bug reports.
//
// Built for a game that crashes a lot, because that is when the logs matter:
//  - it must never stop the game starting: C calls only (nothing here can throw
//    across the loader's constructor), every buffer is bounded, every step that
//    fails is noted and skipped. There is no counterpart of the SEH guard: a
//    process-wide SIGSEGV handler would race Breakpad's for the menu's call;
//  - rename() succeeds on Linux even while another process writes the file; that
//    process would go on writing into the archive, and the next start's
//    retention would delete the folder under it. So every game holds a shared
//    flock() on <data>/.tpf2mp_game.lock for as long as it lives (LaGameLock),
//    and a start that cannot lock it exclusively copies the data folder's logs
//    instead of moving them, as Windows does for a file it cannot move: another
//    game of this data folder is alive (a second instance, or a crashed one not
//    gone yet). The lock needs no access to the other process. A /proc/<pid>/exe
//    scan does (ptrace read), and Snap Steam's game runs confined: AppArmor
//    profile snap.steam.steam in enforce mode, the kernel mediates ptrace ("read
//    trace"), the profile has no ptrace allow rule, and system-observe, the
//    interface that grants "ptrace (read)", is not connected. That the scan then
//    sees no other game is INFERRED (nothing was run inside the snap), so the
//    scan is only the fallback for a filesystem without flock (ENOLCK, EINVAL);
//  - a rename that fails is a copy. After EXDEV (TPF2MP_DATADIR on another
//    filesystem) the source is removed once it was copied whole and unchanged:
//    the next run's writers append to it (fopen "ab", io.open "a");
//  - two starts in quick succession take turns: flock() on logs/.archive.lock,
//    bounded wait (the Windows build uses a named mutex);
//  - the time and disk one start can cost are bounded: logs keep their last
//    TPF2_LOG_TAIL_BYTES, a dump over TPF2_DUMP_MAX_BYTES is skipped, at most
//    TPF2_DUMPS_PER_RUN dumps are copied (whole: a cut minidump is useless), and
//    all copying stops at TPF2_COPY_BUDGET_BYTES; moves are renames and cost nothing;
//  - a crash loop cannot fill the disk: each start keeps at most TPF2_LOG_KEEP
//    archives of its kind, and a half-written archive is pruned like any other.
//
// Folders are created 0700 and copies 0600: the lobby logs carry IP addresses.
// Header-only so the loader's build stays single-file.
#pragma once
#include <dirent.h>
#include <elf.h>
#include <fnmatch.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "datadir_linux.h"

static const uint64_t TPF2_LOG_TAIL_BYTES    = 32ull << 20;
static const uint64_t TPF2_DUMP_MAX_BYTES    = 64ull << 20;
static const uint64_t TPF2_COPY_BUDGET_BYTES = 200ull << 20;
static const uint64_t TPF2_STATE_TAIL_BYTES  = 8ull << 20;
static const int      TPF2_DUMPS_PER_RUN     = 3;
static const int      TPF2_LOG_KEEP          = 5;       // per kind: "previous" runs, "now" copies
static const int      TPF2_LOG_LOCK_WAIT_MS  = 10000;

struct Tpf2mpLogArchive {
    char folder[PATH_MAX];   // the folder written (empty when nothing was found)
    char root[PATH_MAX];     // .../tpf2mp/logs/ (trailing slash)
    int files;               // files placed in the folder
    int skipped;             // files that could not be moved or copied
};

// vsnprintf that refuses to truncate: a cut path must never be used.
__attribute__((format(printf, 3, 4)))
static inline bool LaFmt(char* out, size_t cch, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cch, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cch) { if (cch) out[0] = 0; return false; }
    return true;
}

__attribute__((format(printf, 2, 3)))
static inline void LaNote(FILE* f, const char* fmt, ...)
{
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

static inline bool LaIsFile(const char* p, struct stat* st)
{
    return stat(p, st) == 0 && S_ISREG(st->st_mode);
}

static inline bool LaNewer(const timespec& a, const timespec& b)
{
    return a.tv_sec != b.tv_sec ? a.tv_sec > b.tv_sec : a.tv_nsec > b.tv_nsec;
}

// Copies src to dst while another process may still be writing it. A file over
// tailCap (0: no cap) keeps its tail: the end of a log is what a bug report
// needs. Reads stop at the size seen on open, so a log that keeps growing
// cannot hold the start up. *copied is the number of bytes written.
static inline bool LaCopy(const char* src, const char* dst, uint64_t tailCap, uint64_t* copied)
{
    *copied = 0;
    int in = open(src, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (in < 0) return false;
    struct stat st;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) { close(in); return false; }
    uint64_t size = (uint64_t)st.st_size, start = 0;
    if (tailCap && size > tailCap) start = size - tailCap;
    if (start && lseek(in, (off_t)start, SEEK_SET) != (off_t)start) { close(in); return false; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOCTTY, 0600);
    if (out < 0) { close(in); return false; }
    const size_t chunk = 1 << 20;
    char* buf = (char*)malloc(chunk);
    bool ok = buf != nullptr;
    uint64_t left = size - start;
    while (ok && left > 0) {
        ssize_t got = read(in, buf, left < chunk ? (size_t)left : chunk);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ok = got == 0; break; }   // shrank under us: keep what was read
        left -= (uint64_t)got;
        for (ssize_t done = 0; ok && done < got;) {
            ssize_t put = write(out, buf + done, (size_t)(got - done));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) ok = false;
            else { done += put; *copied += (uint64_t)put; }
        }
    }
    const timespec times[2] = { { 0, UTIME_OMIT }, st.st_mtim };
    futimens(out, times);
    free(buf);
    if (close(out) != 0) ok = false;
    close(in);
    return ok;
}

// Read the ELF identity from bounded PT_NOTE records, without executing a helper.
// No game addresses or ABI layouts are involved: these are ELF64 file headers.
static inline void LaBuildId(int fd, char* out, size_t cap)
{
    out[0] = 0;
    Elf64_Ehdr eh = {};
    struct stat st;
    if (fstat(fd, &st) || pread(fd, &eh, sizeof(eh), 0) != sizeof(eh) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum > 1024 ||
        eh.e_phoff > (uint64_t)st.st_size ||
        uint64_t(eh.e_phnum) * sizeof(Elf64_Phdr) > uint64_t(st.st_size) - eh.e_phoff) return;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph = {};
        if (pread(fd, &ph, sizeof(ph), eh.e_phoff + i * sizeof(ph)) != sizeof(ph) ||
            ph.p_type != PT_NOTE || ph.p_filesz > (1u << 20) || ph.p_offset > uint64_t(st.st_size) ||
            ph.p_filesz > uint64_t(st.st_size) - ph.p_offset) continue;
        uint64_t off = 0;
        while (off + sizeof(Elf64_Nhdr) <= ph.p_filesz) {
            Elf64_Nhdr n = {};
            if (pread(fd, &n, sizeof(n), ph.p_offset + off) != sizeof(n)) break;
            off += sizeof(n);
            uint64_t ns = (uint64_t(n.n_namesz) + 3) & ~uint64_t(3);
            uint64_t ds = (uint64_t(n.n_descsz) + 3) & ~uint64_t(3);
            if (ns + ds > ph.p_filesz - off) break;
            char name[4], id[64];
            if (n.n_type == NT_GNU_BUILD_ID && n.n_namesz == 4 && n.n_descsz > 0 &&
                n.n_descsz <= sizeof(id) && 2 * n.n_descsz + 1 <= cap &&
                pread(fd, name, 4, ph.p_offset + off) == 4 && !memcmp(name, "GNU\0", 4) &&
                pread(fd, id, n.n_descsz, ph.p_offset + off + ns) == n.n_descsz) {
                for (unsigned j = 0; j < n.n_descsz; ++j) snprintf(out + 2*j, 3, "%02x", (unsigned char)id[j]);
                return;
            }
            off += ns + ds;
        }
    }
}

static inline void LaNoteModules(FILE* about, const char* dir, const char* mask, const char* shown)
{
    DIR* d = opendir(dir);
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (fnmatch(mask, e->d_name, 0)) continue;
        char path[PATH_MAX], id[129] = "", date[64] = "unknown";
        struct stat st;
        if (!LaFmt(path, sizeof(path), "%s%s", dir, e->d_name) || !LaIsFile(path, &st)) continue;
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (fd >= 0) { LaBuildId(fd, id, sizeof(id)); close(fd); }
        struct tm utc = {};
        if (gmtime_r(&st.st_mtime, &utc)) strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S UTC", &utc);
        LaNote(about, "  %s/%s: %llu bytes, written %s, GNU build-id %s\n", shown, e->d_name,
               (unsigned long long)st.st_size, date, id[0] ? id : "unavailable");
    }
    closedir(d);
}

// Redact only copies. Walk JSON string tokens so a quoted key in a message is
// not mistaken for a key. Escape bytes become stars too, preserving byte count.
// A tail may start inside a credential: omit its incomplete first line before
// calling this helper. Failure removes the copy rather than leaving secrets.
static inline bool LaRedactFile(const char* path, bool truncated = false)
{
    FILE* f = fopen(path, "r+be");
    if (!f) { unlink(path); return false; }
    struct stat st;
    bool ok = !fstat(fileno(f), &st) && st.st_size >= 0 && uint64_t(st.st_size) <= TPF2_STATE_TAIL_BYTES;
    size_t n = ok ? size_t(st.st_size) : 0;
    char* b = ok ? (char*)malloc(n + 1) : nullptr;
    ok = b && fread(b, 1, n, f) == n;
    if (ok) {
        if (truncated) {
            for (size_t i = 0; i < n && b[i] != '\n'; ++i) b[i] = ' ';
        }
        const char* keys[] = {"code", "cross_code", "steam", "steam_code", "steam_secret", "password", "pass", "passcode", "secret"};
        for (size_t i = 0; i < n;) {
            if (b[i++] != '"') continue;
            size_t start = i;
            while (i < n && b[i] != '"') { if (b[i] == '\\' && i + 1 < n) ++i; ++i; }
            size_t end = i;
            if (i == n) break;
            ++i;
            bool secret = false;
            for (const char* key : keys) if (strlen(key) == end - start && !memcmp(b + start, key, end - start)) secret = true;
            if (!secret) continue;
            size_t j = i;
            auto ws = [&](size_t& k) { while (k < n && (b[k] == ' ' || b[k] == '\t' || b[k] == '\r' || b[k] == '\n')) ++k; };
            ws(j);
            if (j == n || b[j++] != ':') continue;
            ws(j);
            if (j == n || b[j++] != '"') continue;
            while (j < n && b[j] != '"') {
                if (b[j] == '\\' && j + 1 < n) b[j++] = '*';
                b[j++] = '*';
            }
            i = j < n ? j + 1 : j;
        }
        rewind(f);
        ok = fwrite(b, 1, n, f) == n;
    }
    free(b);
    if (fclose(f)) ok = false;
    if (!ok) unlink(path);
    return ok;
}

// yyyymmdd-hhmmss-<kind>[n]: returns the kind (0 = previous, 1 = now), or -1.
static inline int LaArchiveKind(const char* n)
{
    if (strlen(n) < 18) return -1;
    for (int i = 0; i < 15; i++) {
        if (i == 8) { if (n[i] != '-') return -1; }
        else if (n[i] < '0' || n[i] > '9') return -1;
    }
    if (n[15] != '-') return -1;
    if (!strncmp(n + 16, "previous", 8)) return 0;
    if (!strncmp(n + 16, "now", 3)) return 1;
    return -1;
}

// Deletes a folder this code wrote (files only; it never writes subfolders).
static inline void LaRemoveArchive(const char* dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    DIR* d = fdopendir(fd);
    if (!d) { close(fd); return; }
    while (dirent* e = readdir(d)) {
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || S_ISDIR(st.st_mode)) continue;
        unlinkat(fd, e->d_name, 0);
    }
    closedir(d);   // closes fd
    rmdir(dir);
}

// The logs root (trailing slash, created 0700) and the data folder (trailing
// slash). The root sits beside data/ in $XDG_DATA_HOME/tpf2mp; with only
// TPF2MP_DATADIR to go on, inside the data folder.
static inline bool LaLogsRoot(char* root, size_t cch, char* data, size_t dcch)
{
    if (!Tpf2mpDataDirA(data, dcch)) return false;
    char base[PATH_MAX];
    if (Tpf2mpRootDir(base, sizeof(base))) {
        // the parent too: with TPF2MP_DATADIR set nothing else creates it
        if (!Tpf2mpMkdirs(base) || !LaFmt(root, cch, "%s/logs/", base)) return false;
    } else if (!LaFmt(root, cch, "%slogs/", data)) {
        return false;
    }
    if (mkdir(root, 0700) != 0 && errno != EEXIST) return false;
    struct stat st;
    return stat(root, &st) == 0 && S_ISDIR(st.st_mode);
}

// The folder of the running executable with a trailing slash: the game folder
// when called from the game. The loader lives elsewhere, so this stands in for
// the proxy's "folder of this DLL".
static inline bool LaExeDir(char* out, size_t cch)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { if (cch) out[0] = 0; return false; }
    exe[n] = 0;
    char* s = strrchr(exe, '/');
    if (!s) { if (cch) out[0] = 0; return false; }
    s[1] = 0;
    return LaFmt(out, cch, "%s", exe);
}

// Another live TransportFever2 (not this process): the fallback of LaGameLock for
// a data folder on a filesystem without flock. Reading /proc/<pid>/exe needs
// ptrace read access, so another user's game does not count, and inside Snap
// Steam's confinement no game may be seen at all (see the top of this file).
static inline bool LaOtherGameRunning()
{
    DIR* d = opendir("/proc");
    if (!d) return false;
    const long self = (long)getpid();
    bool found = false;
    while (!found) {
        dirent* e = readdir(d);
        if (!e) break;
        char* end = nullptr;
        long pid = strtol(e->d_name, &end, 10);
        if (!end || *end || pid <= 0 || pid == self) continue;
        char link[64], exe[PATH_MAX];
        snprintf(link, sizeof(link), "/proc/%ld/exe", pid);
        ssize_t n = readlink(link, exe, sizeof(exe) - 1);
        if (n <= 0) continue;
        exe[n] = 0;
        const char* base = strrchr(exe, '/');
        base = base ? base + 1 : exe;
        // " (deleted)" follows the name when the file was replaced since
        found = !strncmp(base, "TransportFever2", 15) && (base[15] == 0 || base[15] == ' ');
    }
    closedir(d);
    return found;
}

// The liveness lock of a data folder: every game holds a shared flock() on
// <data>.tpf2mp_game.lock (data with its trailing slash) from its loader's
// constructor to its end. The descriptor is leaked on purpose. The lock goes
// when the last descriptor of it closes, so also after a crash or SIGKILL, and a
// child forked without exec (a dump writer) keeps it until that child ends;
// O_CLOEXEC keeps it out of the programs the game starts (the lobby).
// probe: try it exclusively first, which fails with EWOULDBLOCK exactly when
// another process holds it. flock() converts a lock by dropping it first, so a
// prober holds logs/.archive.lock: no other start probes in between.
// Returns 1 when another process holds the lock, 0 when none does, -1 when that
// cannot be told (not probed, no file, or no flock here: ENOLCK, EINVAL).
static inline int LaGameLock(const char* data, bool probe)
{
    char p[PATH_MAX];
    if (!data || !data[0] || !LaFmt(p, sizeof(p), "%s.tpf2mp_game.lock", data)) return -1;
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOCTTY, 0600);
    if (fd < 0) return -1;
    int other = -1, r;
    if (probe) {
        do r = flock(fd, LOCK_EX | LOCK_NB); while (r != 0 && errno == EINTR);
        if (r == 0) other = 0;
        else if (errno == EWOULDBLOCK) other = 1;
    }
    // A start that gave up on logs/.archive.lock can meet a prober's exclusive
    // lock, which is converted right away: a few tries over 100 ms.
    const timespec step = { 0, 10 * 1000000L };
    for (int i = 0; i <= 10; i++) {
        if (flock(fd, LOCK_SH | LOCK_NB) == 0) return other;   // fd leaked on purpose
        const int e = errno;
        if (e != EWOULDBLOCK && e != EINTR) break;
        if (e == EWOULDBLOCK) nanosleep(&step, nullptr);
    }
    close(fd);
    return other;
}

// The crash_dump folder (trailing slash) of the Steam account that ran the game
// last: the newest stdout.txt under every Steam folder the game may have come
// from. There is no registry on Linux, so the candidates are, deduplicated by
// realpath:
//   <game>/../../..                  the library holding the game; the Steam
//                                    folder itself for the default library
//   $STEAM_COMPAT_CLIENT_INSTALL_PATH INFERRED: set by Steam for launches
//   $XDG_DATA_HOME/Steam, $HOME/.local/share/Steam   native, and Snap (its HOME
//                                    is ~/snap/steam/common)
//   $HOME/.steam/root, $HOME/.steam/steam            Steam's own links to itself
//   $HOME/.steam/debian-installation                 Debian's steam-installer
//   $HOME/snap/steam/common/.local/share/Steam       Snap, seen from outside it
//   $HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam   Flatpak (INFERRED)
static inline bool LaGameCrashDump(const char* gameDir, char* best, size_t cch)
{
    best[0] = 0;
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* compat = getenv("STEAM_COMPAT_CLIENT_INSTALL_PATH");
    if (home && home[0] != '/') home = nullptr;
    if (xdg && xdg[0] != '/') xdg = nullptr;
    if (compat && compat[0] != '/') compat = nullptr;

    enum { kMax = 10 };
    char cand[kMax][PATH_MAX];
    int nc = 0;
    auto add = [&](const char* a, const char* b) {
        char p[PATH_MAX];
        if (!a || nc >= kMax || !LaFmt(p, sizeof(p), "%s%s", a, b)) return;
        if (!realpath(p, cand[nc])) return;
        for (int i = 0; i < nc; i++) if (!strcmp(cand[i], cand[nc])) return;
        nc++;
    };
    if (gameDir && gameDir[0] == '/') add(gameDir, "/../../..");
    add(compat, "");
    add(xdg, "/Steam");
    add(home, "/.local/share/Steam");
    add(home, "/.steam/root");
    add(home, "/.steam/steam");
    add(home, "/.steam/debian-installation");
    add(home, "/snap/steam/common/.local/share/Steam");
    add(home, "/.var/app/com.valvesoftware.Steam/.local/share/Steam");

    timespec bestT = {};
    for (int i = 0; i < nc; i++) {
        char ud[PATH_MAX];
        if (!LaFmt(ud, sizeof(ud), "%s/userdata", cand[i])) continue;
        DIR* d = opendir(ud);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char cd[PATH_MAX], so[PATH_MAX];
            struct stat st;
            if (!LaFmt(cd, sizeof(cd), "%s/%s/1066780/local/crash_dump/", ud, e->d_name)) continue;
            if (!LaFmt(so, sizeof(so), "%sstdout.txt", cd)) continue;
            if (LaIsFile(so, &st) && LaNewer(st.st_mtim, bestT)) {
                bestT = st.st_mtim;
                LaFmt(best, cch, "%s", cd);
            }
        }
        closedir(d);
    }
    return best[0] != 0;
}

// previousSession: run at process start; moves the data folder's logs when
// moveLogs (no other game holds that folder: Tpf2mpArchiveLogsSafe finds out),
// else copies them. Without previousSession it copies everything as it is now.
// gameDir has a trailing slash (LaExeDir), or is null.
static inline bool Tpf2mpArchiveLogs(bool previousSession, const char* gameDir, bool moveLogs, Tpf2mpLogArchive* out)
{
    memset(out, 0, sizeof(*out));
    char data[PATH_MAX];
    if (!LaLogsRoot(out->root, sizeof(out->root), data, sizeof(data))) { out->root[0] = 0; return false; }

    char keepPath[PATH_MAX];
    const bool keepLogs = LaFmt(keepPath, sizeof(keepPath), "%stpf2mp_keep_logs.txt", data) && access(keepPath, F_OK) == 0;
    if (keepLogs) moveLogs = false;

    // The existing archives, by kind; the newest one's time bounds the dumps.
    // A folder's mtime is when its last file was placed: the end of that archive.
    timespec since = {};
    char names[2][64][32];
    size_t count[2] = { 0, 0 };
    if (DIR* d = opendir(out->root)) {
        while (dirent* e = readdir(d)) {
            int kind = LaArchiveKind(e->d_name);
            char p[PATH_MAX];
            struct stat st;
            if (kind < 0 || !LaFmt(p, sizeof(p), "%s%s", out->root, e->d_name)) continue;
            if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (LaNewer(st.st_mtim, since)) since = st.st_mtim;
            if (count[kind] < 64 && strlen(e->d_name) < 32) strcpy(names[kind][count[kind]++], e->d_name);
        }
        closedir(d);
    }
    if (since.tv_sec == 0 && since.tv_nsec == 0) {   // no archive yet: dumps of the last day
        clock_gettime(CLOCK_REALTIME, &since);
        since.tv_sec -= 24 * 3600;
    }

    time_t now = time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    const int myKind = previousSession ? 0 : 1;
    const char* tag = previousSession ? "previous" : "now";
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &lt);
    // Retention trusts that names sort by time. Within one second the suffix goes
    // on from the highest one of this stamp still present: a pruned name must not
    // be taken again, or the newest archive would sort first. (Found by the
    // off-game test; the Windows loop starts at the bare name.)
    int firstK = 1;
    {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s-%s", stamp, tag);
        const size_t pl = strlen(prefix);
        for (size_t i = 0; i < count[myKind]; i++) {
            const char* nm = names[myKind][i];
            if (strncmp(nm, prefix, pl)) continue;
            int k = nm[pl] == 0 ? 1 : (nm[pl] >= '2' && nm[pl] <= '9' && nm[pl + 1] == 0) ? nm[pl] - '0' : 0;
            if (k >= firstK) firstK = k + 1;
        }
    }
    bool made = false;
    for (int k = firstK; !made && k < 10; k++) {
        bool fits = k == 1 ? LaFmt(out->folder, sizeof(out->folder), "%s%s-%s", out->root, stamp, tag)
                           : LaFmt(out->folder, sizeof(out->folder), "%s%s-%s%d", out->root, stamp, tag, k);
        if (!fits) break;
        made = mkdir(out->folder, 0700) == 0;
    }
    if (!made) { out->folder[0] = 0; return false; }

    char aboutPath[PATH_MAX];
    FILE* about = LaFmt(aboutPath, sizeof(aboutPath), "%s/about.txt", out->folder) ? fopen(aboutPath, "wbe") : nullptr;
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &lt);
    LaNote(about, "TpF2 Multiplayer logs, gathered %s (%s)\n", when,
           previousSession ? "the previous run, saved when the game started again" : "the running game, copied while it runs");
    LaNote(about, "Paths below are relative: data = $XDG_DATA_HOME/tpf2mp/data (~/.local/share when unset), "
                  "netpunch = the lobby folder beside it, game = the Transport Fever 2 folder, "
                  "crash_dump = <Steam>/userdata/<account>/1066780/local/crash_dump.\n\n");

    char install[PATH_MAX] = "", path[PATH_MAX], version[256] = "unknown";
    Tpf2mpRootDir(install, sizeof(install));
    // Native installers record their version in the manifest, not the game dir.
    if (install[0] && LaFmt(path, sizeof(path), "%s/tpf2mp_install.txt", install)) {
        if (FILE* f = fopen(path, "re")) {
            char line[512];
            while (fgets(line, sizeof(line), f)) if (!strncmp(line, "version\t", 8)) {
                snprintf(version, sizeof(version), "%.255s", line + 8); break;
            }
            fclose(f);
        }
    }
    if (!strcmp(version, "unknown") && gameDir && LaFmt(path, sizeof(path), "%stpf2mp_version.txt", gameDir)) {
        if (FILE* f = fopen(path, "re")) { if (!fgets(version, sizeof(version), f)) strcpy(version, "unknown"); fclose(f); }
    }
    version[strcspn(version, "\r\n")] = 0;
    char zone[80];
    strftime(zone, sizeof(zone), "%Z (UTC%z)", &lt);
    struct utsname os = {};
    uname(&os);
    LaNote(about, "TpF2 Multiplayer version %s\nLinux %s %s; log time zone %s\nModules on disk\n", version, os.release, os.machine, zone);
    if (gameDir && gameDir[0]) LaNoteModules(about, gameDir, "TransportFever2", "game");
    if (install[0]) {
        if (LaFmt(path, sizeof(path), "%s/", install)) LaNoteModules(about, path, "*.so", "mod");
        if (LaFmt(path, sizeof(path), "%s/plugins/", install)) LaNoteModules(about, path, "*.so", "mod/plugins");
        if (LaFmt(path, sizeof(path), "%s/netpunch/", install)) LaNoteModules(about, path, "netpunch", "netpunch");
    }
    LaNote(about, "\nFiles\n");

    // Logs held by another live game are copied: a rename would leave that game
    // writing into this archive.
    const bool moveData = previousSession && moveLogs;
    if (keepLogs) LaNote(about, "  tpf2mp_keep_logs.txt: copy logs and retain all archives\n");
    else if (previousSession && !moveData)
        LaNote(about, "  another Transport Fever 2 is still running: the data folder's logs are copied, not moved\n");

    uint64_t budget = TPF2_COPY_BUDGET_BYTES;
    // A copy within the budget. *seen: the source before the copy (zeroed when it
    // is no file), *copied: the bytes written. Notes a failure only.
    auto copyBudgeted = [&](const char* src, const char* dst, const char* shown, uint64_t tailCap,
                            struct stat* seen, uint64_t* copied) -> bool {
        *copied = 0;
        if (!LaIsFile(src, seen)) memset(seen, 0, sizeof(*seen));
        const uint64_t size = (uint64_t)seen->st_size;
        const uint64_t need = tailCap && size > tailCap ? tailCap : size;
        if (need > budget) {
            LaNote(about, "  %-52s skipped: this archive's copy budget is used up\n", shown);
            return false;
        }
        const bool ok = LaCopy(src, dst, tailCap, copied);
        budget -= *copied < budget ? *copied : budget;
        if (!ok) LaNote(about, "  %-52s could not be read\n", shown);
        return ok;
    };
    auto noteCopied = [&](const char* shown, uint64_t size, uint64_t tailCap, const char* why) {
        LaNote(about, "  %-52s %12llu bytes%s%s\n", shown, (unsigned long long)size,
               tailCap && size > tailCap ? (tailCap == TPF2_STATE_TAIL_BYTES ? " (only the last 8 MB kept)" : " (only the last 32 MB kept)") : "", why);
    };
    auto place = [&](const char* src, const char* destName, const char* shown, bool move, uint64_t tailCap) {
        char dst[PATH_MAX];
        bool ok = false;
        struct stat seen;
        uint64_t copied = 0;
        if (!LaFmt(dst, sizeof(dst), "%s/%s", out->folder, destName)) {
            LaNote(about, "  %-52s skipped: path too long\n", shown);
        } else if (move) {
            struct stat st;
            const uint64_t size = LaIsFile(src, &st) ? (uint64_t)st.st_size : 0;
            ok = rename(src, dst) == 0;   // a rename only: never a copy that could half-succeed
            const int e = ok ? 0 : errno;
            if (ok) {
                LaNote(about, "  %-52s %12llu bytes\n", shown, (unsigned long long)size);
            } else if ((ok = copyBudgeted(src, dst, shown, tailCap, &seen, &copied))) {
                // Not moved, so copied: otherwise the next run's libraries recreate
                // it empty. After EXDEV the source goes too when the copy is whole
                // and the file did not change since (a move means no other game
                // holds this data folder); anything else keeps it.
                struct stat after;
                const bool whole = e == EXDEV && copied == (uint64_t)seen.st_size &&
                                   (!tailCap || (uint64_t)seen.st_size <= tailCap) &&
                                   stat(src, &after) == 0 && after.st_dev == seen.st_dev && after.st_ino == seen.st_ino &&
                                   after.st_size == seen.st_size && after.st_mtim.tv_sec == seen.st_mtim.tv_sec &&
                                   after.st_mtim.tv_nsec == seen.st_mtim.tv_nsec;
                const bool removed = whole && unlink(src) == 0;
                noteCopied(shown, (uint64_t)seen.st_size, tailCap,
                           removed ? " (on another filesystem: copied, then removed)" : " (could not be moved: copied)");
            }
        } else if ((ok = copyBudgeted(src, dst, shown, tailCap, &seen, &copied))) {
            noteCopied(shown, (uint64_t)seen.st_size, tailCap, "");
        }
        if (ok) out->files++; else out->skipped++;
        return ok;
    };
    auto each = [&](const char* dir, const char* suffix, const char* prefix, const char* shownDir, bool move, uint64_t cap = TPF2_LOG_TAIL_BYTES, bool redact = false) {
        DIR* d = opendir(dir);
        if (!d) return;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.' || fnmatch(suffix, e->d_name, 0) ||
                (cap == TPF2_STATE_TAIL_BYTES && (!strncmp(e->d_name, "incoming_save.", 14) ||
                 !strncmp(e->d_name, "terrain_", 8)))) continue;
            char src[PATH_MAX], dest[PATH_MAX], shown[PATH_MAX];
            struct stat st;
            if (!LaFmt(src, sizeof(src), "%s%s", dir, e->d_name) || !LaIsFile(src, &st)) continue;
            if (!LaFmt(dest, sizeof(dest), "%s%s", prefix, e->d_name)) continue;
            if (!LaFmt(shown, sizeof(shown), "%s/%s", shownDir, e->d_name)) continue;
            const bool placed = place(src, dest, shown, move, cap);
            if (redact && LaFmt(src, sizeof(src), "%s/%s", out->folder, dest)) {
                if (!placed) { unlink(src); continue; }
                if (!LaRedactFile(src, uint64_t(st.st_size) > cap)) {
                    --out->files; ++out->skipped;
                    LaNote(about, "  %s removed: redaction failed\n", shown);
                }
            }
        }
        closedir(d);
    };

    // 1. the data folder: loader, bridge, menu, slice, plugin host, company logs
    each(data, "*.log", "", "data", moveData);
    // 2. the lobby: beside the data folder, and the game folder's (the Lua side's fallback)
    char base[PATH_MAX], net[PATH_MAX];
    if (Tpf2mpRootDir(base, sizeof(base)) && LaFmt(net, sizeof(net), "%s/netpunch/", base))
        each(net, "*.log", "netpunch_", "netpunch", false);
    if (gameDir && gameDir[0] && LaFmt(net, sizeof(net), "%snetpunch/", gameDir))
        each(net, "*.log", "game_netpunch_", "game/netpunch", false);

    // 3. the game's own log and crash dumps (the Steam account that ran the game last)
    char best[PATH_MAX];
    if (LaGameCrashDump(gameDir, best, sizeof(best))) {
        char so[PATH_MAX];
        struct stat st;
        if (LaFmt(so, sizeof(so), "%sstdout.txt", best))
            place(so, "game_stdout.txt", "game log (stdout.txt, includes the Lua script)", false, TPF2_LOG_TAIL_BYTES);
        if (previousSession && LaFmt(so, sizeof(so), "%slockfile", best) && LaIsFile(so, &st))
            LaNote(about, "  crash_dump/lockfile was still there: the previous run ended without the game's own "
                          "shutdown (killed, frozen, or a crash that wrote no dump), or it is still running\n");

        // newest dumps since the last archive
        struct Dump { char name[80]; timespec t; uint64_t size; } dumps[64];
        int nd = 0;
        if (DIR* d = opendir(best)) {
            while (dirent* e = readdir(d)) {
                const size_t nl = strlen(e->d_name);
                char p[PATH_MAX];
                if (e->d_name[0] == '.' || nl <= 4 || nl >= 80 || strcmp(e->d_name + nl - 4, ".dmp")) continue;
                if (!LaFmt(p, sizeof(p), "%s%s", best, e->d_name) || !LaIsFile(p, &st)) continue;
                if (!LaNewer(st.st_mtim, since)) continue;
                Dump dm;
                memcpy(dm.name, e->d_name, nl + 1);
                dm.t = st.st_mtim;
                dm.size = (uint64_t)st.st_size;
                if (nd < 64) dumps[nd++] = dm;
                else {   // keep the 64 newest
                    int oldest = 0;
                    for (int i = 1; i < nd; i++) if (LaNewer(dumps[oldest].t, dumps[i].t)) oldest = i;
                    if (LaNewer(dm.t, dumps[oldest].t)) dumps[oldest] = dm;
                }
            }
            closedir(d);
        }
        for (int i = 0; i < nd; i++)   // newest first
            for (int j = i + 1; j < nd; j++)
                if (LaNewer(dumps[j].t, dumps[i].t)) { Dump t = dumps[i]; dumps[i] = dumps[j]; dumps[j] = t; }
        if (nd > TPF2_DUMPS_PER_RUN)
            LaNote(about, "  %s%d crash dumps since the last archive; the newest %d are here\n",
                   nd == 64 ? "at least " : "", nd, TPF2_DUMPS_PER_RUN);
        for (int i = 0; i < nd && i < TPF2_DUMPS_PER_RUN; i++) {
            char src[PATH_MAX], dest[PATH_MAX], shown[PATH_MAX];
            if (dumps[i].size > TPF2_DUMP_MAX_BYTES) {
                LaNote(about, "  crash dump %-41s skipped: %llu bytes\n", dumps[i].name, (unsigned long long)dumps[i].size);
                out->skipped++;
            } else if (LaFmt(src, sizeof(src), "%s%s", best, dumps[i].name) &&
                       LaFmt(dest, sizeof(dest), "crash_%s", dumps[i].name) &&
                       LaFmt(shown, sizeof(shown), "crash dump %s", dumps[i].name)) {
                place(src, dest, shown, false, 0);   // whole: a cut minidump is useless
            }
        }
    }
    // State is copied after every log/dump, using the remaining copy budget.
    LaNote(about, "\nState files (the last 8 MB of each)\n");
    each(data, "*.txt", "state_", "data", false, TPF2_STATE_TAIL_BYTES);
    each(data, "tpf2*.cfg", "state_", "data", false, TPF2_STATE_TAIL_BYTES);
    auto config = [&](const char* dir, const char* prefix, const char* shown) {
        each(dir, "tpf2*.cfg", prefix, shown, false, TPF2_STATE_TAIL_BYTES);
        each(dir, "tpf2_menu_flags.txt", prefix, shown, false, TPF2_STATE_TAIL_BYTES);
        each(dir, "tpf2mp_version.txt", prefix, shown, false, TPF2_STATE_TAIL_BYTES);
    };
    auto lobbyState = [&](const char* dir, const char* prefix, const char* shown) {
        each(dir, "*.jsonl", prefix, shown, false, TPF2_STATE_TAIL_BYTES, true);
        each(dir, "*.json", prefix, shown, false, TPF2_STATE_TAIL_BYTES, true);
        each(dir, "*.txt", prefix, shown, false, TPF2_STATE_TAIL_BYTES, true);
    };
    if (install[0]) {
        if (LaFmt(path, sizeof(path), "%s/", install)) config(path, "mod_", "mod");
        if (LaFmt(path, sizeof(path), "%s/plugins/", install)) each(path, "*.cfg", "mod_plugins_", "mod/plugins", false, TPF2_STATE_TAIL_BYTES);
        if (LaFmt(path, sizeof(path), "%s/netpunch/", install)) lobbyState(path, "netpunch_", "netpunch");
    }
    if (gameDir && gameDir[0]) {
        config(gameDir, "game_", "game");
        if (LaFmt(path, sizeof(path), "%splugins/", gameDir)) each(path, "*.cfg", "game_plugins_", "game/plugins", false, TPF2_STATE_TAIL_BYTES);
        if (LaFmt(path, sizeof(path), "%snetpunch/", gameDir)) lobbyState(path, "game_netpunch_", "game/netpunch");
    }
    LaNote(about, "  Invitation codes in lobby copies are masked with *; truncated first lines are blanked.\n");
    if (about) fclose(about);

    if (out->files == 0) {
        LaRemoveArchive(out->folder);
        out->folder[0] = 0;
        return false;
    }

    // Keep the newest TPF2_LOG_KEEP of this kind, this one included (names sort
    // by time). A folder a killed start left half-written goes the same way.
    size_t n = count[myKind];
    if (!keepLogs && n + 1 > (size_t)TPF2_LOG_KEEP) {
        char (*list)[32] = names[myKind];
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++)
                if (strcmp(list[j], list[i]) < 0) { char t[32]; strcpy(t, list[i]); strcpy(list[i], list[j]); strcpy(list[j], t); }
        size_t drop = n + 1 - TPF2_LOG_KEEP;
        for (size_t i = 0; i < drop && i < n; i++) {
            char p[PATH_MAX];
            if (LaFmt(p, sizeof(p), "%s%s", out->root, list[i])) LaRemoveArchive(p);
        }
    }
    return true;
}

// The entry point both callers use: the loader's constructor (previousSession)
// and a menu action off the render thread (a copy of the running game's logs).
// Starts in quick succession (a relaunch after a crash, a second instance) take
// turns on the same folders through flock() on logs/.archive.lock; a wait longer
// than 10 s gives up rather than delay the game. A filesystem without flock
// goes on unlocked, as the Windows build does when it gets no mutex.
// previousSession also takes this process's liveness lock on the data folder
// (LaGameLock), probed under the archive lock; a start that gave up waiting
// still takes it, so later starts leave this run's logs alone.
static inline bool Tpf2mpArchiveLogsSafe(bool previousSession, const char* gameDir, Tpf2mpLogArchive* out)
{
    memset(out, 0, sizeof(*out));
    char root[PATH_MAX], data[PATH_MAX], lockPath[PATH_MAX];
    int lock = -1;
    if (LaLogsRoot(root, sizeof(root), data, sizeof(data)) && LaFmt(lockPath, sizeof(lockPath), "%s.archive.lock", root))
        lock = open(lockPath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOCTTY, 0600);
    // the data folder on its own: its liveness lock matters without a logs folder too
    const bool gameLock = previousSession && Tpf2mpDataDirA(data, sizeof(data));
    if (lock >= 0) {
        timespec t0 = {}, t = {};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (flock(lock, LOCK_EX | LOCK_NB) != 0) {
            if (errno != EWOULDBLOCK && errno != EINTR) { close(lock); lock = -1; break; }
            clock_gettime(CLOCK_MONOTONIC, &t);
            if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 >= TPF2_LOG_LOCK_WAIT_MS) {
                close(lock);
                if (gameLock) LaGameLock(data, false);
                return false;
            }
            const timespec step = { 0, 50 * 1000000L };
            nanosleep(&step, nullptr);
        }
    }
    const int other = gameLock ? LaGameLock(data, true) : -1;
    const bool moveLogs = previousSession && (other == 0 || (other < 0 && !LaOtherGameRunning()));
    const bool ok = Tpf2mpArchiveLogs(previousSession, gameDir, moveLogs, out);
    if (lock >= 0) { flock(lock, LOCK_UN); close(lock); }
    return ok;
}
