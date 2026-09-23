// codewrite_linux.h -- write bytes into this process's memory (the game's code
// or RELRO data) through /proc/self/mem, without changing a page protection.
//
// Why: the original Linux libraries patched the game with mprotect(rwx) / write /
// mprotect(r-x) on whole pages (hook_posix.cpp InstallHook, near_alloc.h
// Tpf2mpRedirectCall, setplayer_linux.cpp), and nothing serialises them across
// libraries. boot.cpp loads the bridge, menu, slice and plugin host one after
// another; the first three patch from init threads they start, and plugins
// patch from the host's constructor, so all of them can run at once. When two
// writes share a 4 KB page, one library can put the page back to r-x between
// the other's mprotect and its write, and that write faults.
//
// A write to /proc/self/mem takes the kernel's forced-access path (FOLL_FORCE),
// the one a debugger uses for a breakpoint: a private read-only mapping gets its
// copy-on-write page, as after mprotect + write, and the mapping's protection is
// never touched. A writer then needs no protection change and cannot undo
// another writer's.
//
// Evidence (2026-09-12, kernel 7.0.0-31-generic, plugin-host scratch
// fix/probe.cpp):
//   - /boot/config-7.0.0-31-generic: CONFIG_PROC_MEM_ALWAYS_FORCE=y. With
//     PROC_MEM_FORCE_PTRACE or PROC_MEM_NO_FORCE instead, or with the
//     proc_mem.force_override= boot parameter, the kernel refuses such a write,
//     and the caller gets TPF2MP_CW_UNAVAILABLE.
//   - The probe opened /proc/self/mem O_RDWR, pwrite of one byte into its own
//     r-xp text returned 1, and the mapping stayed r-xp. The result was the same
//     under AppArmor snap.steam.steam (aa-exec -p snap.steam.steam; label
//     "snap.steam.steam//&unconfined (enforce)").
//   - A non-dumpable process (prctl PR_SET_DUMPABLE 0) gets EACCES from open,
//     because /proc/<pid>/mem then belongs to root. The pressure-vessel
//     processes of the running Snap Steam were dumpable: their /proc/<pid>/mem
//     was owned by the user. A descriptor opened earlier still works after the
//     process becomes non-dumpable, so open it early and keep it.
// Verified inside the build-35924 game under Snap Steam on 2026-09-14:
// all bridge, menu and slice code patches installed through this writer.
//
// Shared by all Linux patchers. A refusal leaves the feature disabled: there is
// no page-protection fallback, because separate DSOs cannot serialize it with
// a local mutex. This does not make patching a currently executing instruction
// safe; hooks must still be installed before their game entry points run.
#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

enum Tpf2mpCodeWriteResult {
    TPF2MP_CW_OK = 0,             // written in full, and read back equal
    TPF2MP_CW_UNAVAILABLE,        // nothing written: bad arguments, a bad fd, or the kernel refused
    TPF2MP_CW_PARTIAL_RESTORED,   // only a prefix was written; the original bytes are back (read back equal)
    TPF2MP_CW_PARTIAL_BROKEN,     // only a prefix was written, and writing the original bytes back failed
    TPF2MP_CW_READBACK,           // written in full, but reading back failed or gave other bytes
};

// open("/proc/self/mem", O_RDWR | O_CLOEXEC): the descriptor, or -1 with errno.
// It addresses the memory of the process that opened it, including after a
// fork: a child must open its own (compare getpid() with the opener's).
static inline int Tpf2mpOpenSelfMem()
{
    int fd;
    do fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC); while (fd < 0 && errno == EINTR);
    return fd;
}

// pread/pwrite of `len` bytes at virtual address `addr`, continuing after short
// counts. Returns the bytes done; when fewer than `len`, *err says why (EIO when
// the kernel just stopped, which is what /proc/<pid>/mem does at a page it
// cannot reach).
static inline size_t Tpf2mpMemXfer(int fd, bool write, uintptr_t addr, void* buf, size_t len, int* err)
{
    size_t done = 0;
    *err = 0;
    while (done < len) {
        const off_t off = (off_t)(addr + done);
        const ssize_t n = write ? pwrite(fd, (const char*)buf + done, len - done, off)
                                : pread(fd, (char*)buf + done, len - done, off);
        if (n > 0) { done += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        *err = n < 0 ? errno : EIO;
        break;
    }
    return done;
}

// Write `len` bytes at `addr` through `memFd` (Tpf2mpOpenSelfMem). The original
// bytes are read first, so a short write can be undone. *err (optional) holds
// the errno behind any result but TPF2MP_CW_OK (0 when bytes merely differed).
static inline int Tpf2mpCodeWrite(int memFd, uintptr_t addr, const uint8_t* bytes, size_t len, int* err)
{
    int e = 0;
    if (err) *err = 0;
    if (memFd < 0 || !bytes || !len || addr + len < addr || addr + len > (uintptr_t)INT64_MAX
        || len > SIZE_MAX / 2) {
        if (err) *err = EINVAL;
        return TPF2MP_CW_UNAVAILABLE;
    }
    // The original and the read-back copy. On the stack for a hook's few
    // bytes; the heap otherwise, never an exception (no C++ unwinding into a
    // plugin or the game).
    uint8_t small[128];
    const bool heap = len > sizeof(small) / 2;
    uint8_t* const mem = heap ? (uint8_t*)malloc(len * 2) : small;
    if (!mem) {
        if (err) *err = ENOMEM;
        return TPF2MP_CW_UNAVAILABLE;
    }
    uint8_t* const orig = mem;
    uint8_t* const back = mem + len;
    int result;
    if (Tpf2mpMemXfer(memFd, false, addr, orig, len, &e) != len) {
        result = TPF2MP_CW_UNAVAILABLE;                   // unreadable here: write nothing
    } else {
        // pwrite does not modify the buffer; Tpf2mpMemXfer takes one pointer for both ways.
        const size_t wrote = Tpf2mpMemXfer(memFd, true, addr, (void*)(uintptr_t)bytes, len, &e);
        if (wrote == 0) {
            result = TPF2MP_CW_UNAVAILABLE;
        } else if (wrote < len) {
            int e2 = 0;
            const bool undone = Tpf2mpMemXfer(memFd, true, addr, orig, wrote, &e2) == wrote
                             && Tpf2mpMemXfer(memFd, false, addr, back, wrote, &e2) == wrote
                             && memcmp(back, orig, wrote) == 0;
            result = undone ? TPF2MP_CW_PARTIAL_RESTORED : TPF2MP_CW_PARTIAL_BROKEN;
        } else {
            __builtin___clear_cache((char*)addr, (char*)(addr + len));
            e = 0;
            const bool same = Tpf2mpMemXfer(memFd, false, addr, back, len, &e) == len
                           && memcmp(back, bytes, len) == 0;
            result = same ? TPF2MP_CW_OK : TPF2MP_CW_READBACK;
        }
    }
    if (heap) free(mem);
    if (err && result != TPF2MP_CW_OK) *err = e;
    return result;
}

// The same through a descriptor opened and closed for this one write, for a
// library that patches a handful of sites and keeps no descriptor. *err holds
// open's errno when the file cannot be opened (TPF2MP_CW_UNAVAILABLE).
static inline int Tpf2mpCodeWriteSelf(uintptr_t addr, const uint8_t* bytes, size_t len, int* err)
{
    const int fd = Tpf2mpOpenSelfMem();
    if (fd < 0) {
        if (err) *err = errno;
        return TPF2MP_CW_UNAVAILABLE;
    }
    const int r = Tpf2mpCodeWrite(fd, addr, bytes, len, err);
    close(fd);
    return r;
}
