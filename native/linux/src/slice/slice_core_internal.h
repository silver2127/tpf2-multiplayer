// slice_core_internal.h -- what slice_core.cpp, slice_install.cpp and
// slice_core_main.cpp share, and the seams the off-game tests drive: the image,
// the data dir and the executable ranges come in through SliceCoreEnv instead of
// being read from the process, so a test can point slice-core at a fake image.
// Not for the decoder areas: their interface is slice_core.h.
#pragma once
#include "slice_core.h"
#include <sys/types.h>
#include <cstddef>
#include <cstdint>

// CommandList::Add (C-ADD-1..4) and the game's Connection() (C-ADD-8, C-ADD-10).
static const uintptr_t kSliceRvaAdd = 0x15da840;
static const int kSliceStealAdd = 14;
static const uintptr_t kSliceRvaConnectionCtor = 0x3190240;
extern const uint8_t kSliceAddPrologue[14];
extern const uint8_t kSliceConnectionCtorBytes[51];
// Build 35924's .text (readelf -S: [13] .text addr 0x6de300 size 0x37adaa5). A
// `done` invoker is only called inside it (C-ADD-7): the first R+X LOAD segment
// also holds .rodata, .gcc_except_table and .eh_frame, and the second one .bind.
static const uintptr_t kSliceRvaTextLo = 0x6de300;
static const uintptr_t kSliceRvaTextHi = 0x3e8bda5;

struct SliceExecRange { uintptr_t lo, hi; };
struct SliceCoreEnv {
    uintptr_t base;
    bool buildOk;
    const char* dataDir;      // with a trailing slash
    const char* rootDir;      // Tpf2mpRootDir(), no trailing slash; "" when there is none
    SliceExecRange exec[8];   // the game image's executable PT_LOAD segments
    int nExec;
    SliceExecRange code[4];   // where a `done` invoker may lie: the game's .text (empty: never fired)
    int nCode;
};

// ---- slice_core.cpp ----
enum class SliceOpenResult : uint8_t { Ok, NoLog, Locked, NoLock };
// Takes the per-data-dir lock (a second game on the same data dir stays inert,
// as the Windows named mutex made it), then opens tpf2_slice.log truncated.
SliceOpenResult SliceCoreOpen(const SliceCoreEnv& env);
const SliceCoreEnv& SliceCoreEnvGet();
bool SliceInExec(uintptr_t addr);
bool SliceInGameCode(uintptr_t addr);     // inside env.code

enum SliceReadMech { kSliceReadNone = 0, kSliceReadVm = 1, kSliceReadPipe = 2 };
// One mechanism's self-test (C-READ-2): a readable range reads fully and
// correctly, address 0, a never-mapped low page and a PROT_NONE page fail, and a
// read straddling into the PROT_NONE page does not succeed.
bool SliceReadSelfTest(int mech, char* why, size_t cap);
void SliceReadSelect(int mech);
int SliceReadMechanism();
bool SliceReadInit();     // self-tests in order, selects the first that passes, logs

// Table and image consistency, before anything is registered: the factory table,
// the script return-address set, the armable call-site set (C-PTR-5), and in the
// image every script return address preceded by an E8 call to a table factory and
// every armable window byte-identical to the analysed one.
bool SliceCoreStaticChecks();
void SliceSessionCacheReset();

// Tests only: the write(2) the inject writer calls (nullptr restores write).
using SliceWriteFn = ssize_t (*)(int fd, const void* buf, size_t n);
void SliceInjectSetWriteFn(SliceWriteFn fn);

// ---- slice_install.cpp ----
void SliceCoreBeginRegistration();
void SliceCoreEndRegistration();
// The SLICE_AREA entries, sorted by name; returns how many ran, or -1 when there
// are more than the registry holds (logged; nothing registered, nothing patched).
int SliceCoreRunRegistrations();
struct SliceInstallReport {
    bool connectionOk;
    bool addInstalled;
    bool ready; // all requested registrations/hooks and the cancel point succeeded
    int factoriesInstalled, factoriesSkipped;
    int hooksInstalled, hooksSkipped;
};
SliceInstallReport SliceCoreInstall();
void SliceCoreLogAlive();
