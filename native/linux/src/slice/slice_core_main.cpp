// slice_core_main.cpp -- tpf2_slice.so's entry point: the library constructor
// starts the init thread, which is slice_hook.cpp's DllMain + Init on Linux.
// libtpf2mp_boot.so dlopens this library from its own constructor, before main().
//
// Init order, each step a precondition of the next:
//   1. the data dir (nowhere to log or write without it: stay inert);
//   2. the per-data-dir lock, then tpf2_slice.log truncated (SliceCoreOpen);
//   3. the build gate: any build-id but 35924's patches nothing (C-BUILD-1);
//   4. the guarded-reader self-test (C-READ-2);
//   5. the table checks against the image (SliceCoreStaticChecks);
//   6. the SLICE_AREA registrations, then the install in its fixed order
//      (slice_install.cpp: Connection() and Add first, C-HOOK-4);
//   7. an alive line every 15 s.
// TPF2MP_NO_PATCHES=1 stops after step 5, as it does for the bridge.
//
// Installing from this thread cannot race a running Add or factory: they run
// only once the title menu or a game exists (C-HOOK-3, INFERRED from the callers
// being UI and script code).
#include "slice_core_internal.h"
#include "train_order_linux.h"
#include "movement_linux.h"
#include "../datadir_linux.h"
#include "../game_image.h"
#include "../slice_ready_linux.h"
#include <link.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

static bool IsGameProcess()
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    const char* base = strrchr(exe, '/');
    return base && strcmp(base + 1, "TransportFever2") == 0;
}

// The main executable's PF_X PT_LOAD segments (dl_iterate_phdr reports it first).
// A `done` invoker is only called when it lies in one of them.
static int CollectExec(dl_phdr_info* info, size_t, void* data)
{
    auto* env = static_cast<SliceCoreEnv*>(data);
    for (int i = 0; i < info->dlpi_phnum && env->nExec < 8; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
        env->exec[env->nExec].lo = info->dlpi_addr + ph.p_vaddr;
        env->exec[env->nExec].hi = info->dlpi_addr + ph.p_vaddr + ph.p_memsz;
        env->nExec++;
    }
    return 1;
}

static void InitThread()
{
    static char dataDir[4096], rootDir[4096];   // referenced by the core for the process lifetime
    if (!Tpf2mpDataDirA(dataDir, sizeof(dataDir))) return;
    if (!Tpf2mpRootDir(rootDir, sizeof(rootDir))) rootDir[0] = 0;

    const Tpf2GameImage img = Tpf2mpGameImage();
    SliceCoreEnv env = {};
    env.base = img.base;
    env.buildOk = img.buildOk;
    env.dataDir = dataDir;
    env.rootDir = rootDir;
    dl_iterate_phdr(CollectExec, &env);
    // Where a `done` invoker may lie: build 35924's .text, and only when that range
    // is inside one of the executable segments just collected.
    if (img.buildOk && img.base) {
        const SliceExecRange text = { img.base + kSliceRvaTextLo, img.base + kSliceRvaTextHi };
        for (int i = 0; i < env.nExec && !env.nCode; i++)
            if (text.lo >= env.exec[i].lo && text.hi <= env.exec[i].hi) env.code[env.nCode++] = text;
    }
    if (SliceCoreOpen(env) != SliceOpenResult::Ok) return;
    if (!SlicePublishReady(dataDir, false)) {
        SliceLog("[slice] cannot initialize hook readiness -- hooks stay disabled\n");
        return;
    }

    SliceLog("[slice] pid %d, init thread %d, data dir=%s\n", (int)getpid(), SliceTid(), dataDir);
    SliceLog("[slice] cfg: game folder (cwd), then data dir, matching Windows 0.4.22 Lua\n");
    if (!img.buildOk) {
        SliceLog("[slice] game build mismatch: the GNU build-id is not build 35924's "
                 "(3a0e156390b0e6f1e372051c24802c8493ae454a) -- hooks NOT installed\n");
        return;
    }
    SliceLog("[slice] game build ok: build 35924, base=%lx, %d executable segment(s), .text %s\n",
             (unsigned long)img.base, env.nExec,
             env.nCode ? "inside one" : "NOT inside one -- no done is ever fired from a cancel");

    char letter[8];
    SliceInstance(letter, sizeof(letter));
    SliceLog("[slice] attached, base=%lx instance=%s dumpprop=%d\n", (unsigned long)img.base,
             letter[0] ? letter : "?", SliceDumpPropOn() ? 1 : 0);

    if (!env.nCode || !SliceReadInit()) return;
    if (!SliceCoreStaticChecks()) return;

    const char* noPatches = getenv("TPF2MP_NO_PATCHES");
    if (noPatches && noPatches[0] == '1') {
        SliceLog("[slice] TPF2MP_NO_PATCHES=1 -- no registrations, nothing patched\n");
        return;
    }
    if (SliceCoreRunRegistrations() < 0) return;   // logged: nothing registered, nothing patched
    const SliceInstallReport installed = SliceCoreInstall();
    const bool trainOrderReady = SliceInstallTrainOrder(img.base, rootDir, dataDir);
    const bool movementReady = SliceInstallMovement(img.base, rootDir, dataDir);
    const bool published = SlicePublishReady(dataDir, installed.ready && trainOrderReady && movementReady);
    SliceLog("[slice] multiplayer hook readiness: %s\n",
             installed.ready && trainOrderReady && movementReady && published ? "ready" : "NOT READY -- multiplayer startup refused");

    for (;;) {
        sleep(15);
        SliceCoreLogAlive();
        SliceTrainOrderLogAlive();
    }
}

static void* InitThreadEntry(void*)
{
    InitThread();
    return nullptr;
}

// pthread_create rather than std::thread: this runs inside boot's dlopen, before
// main, and std::thread reports a failed create by throwing std::system_error,
// which would leave the constructor and terminate the game. A failure leaves the
// library inert; the log opens on the init thread, so there is nowhere to say so.
__attribute__((constructor))
static void SliceLoad()
{
    if (!IsGameProcess()) return;
    pthread_t th;
    if (pthread_create(&th, nullptr, InitThreadEntry, nullptr) == 0) pthread_detach(th);
}
