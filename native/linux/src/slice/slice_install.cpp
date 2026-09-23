// slice_install.cpp -- the hook registry of tpf2_slice.so: the CommandList::Add
// detour and its cancel, one generic detour per factory with the decoder areas'
// handlers behind it, the areas' own typed hooks, and the one install path.
// Windows funnels 16 hooks through deferrelay_slice.asm into DeferHandler
// (slice_hook.cpp 3343-4105, 4109-4235); the facts behind every choice below are in
// docs/re/linux/SLICE_CORE.md.
//
// WHY NO ASSEMBLY RELAY
//   Add's SysV signature is fully known (C-ADD-3): five pointer arguments, no r9,
//   no SSE, no stack argument. Its detour is an ordinary typed function.
//   A factory's arguments differ per factory, but for 35 of the 37 they are all in
//   registers: no factory touches a stack argument except SaveGame (0x15ed140
//   reads [rbp+0x10] at 0x15ed176) and Book (0x15ecd80, [rbp+0x10..0x24] at
//   0x15ecdbc-0x15ecde5), and none uses a ymm register (C-FAC-4). A function
//   declared (6 x uint64_t, 8 x __m128) therefore receives every argument of the
//   other 35 exactly as the game passed it, and calling the trampoline with the
//   same list passes them on unchanged, whatever each one means (C-ABI-1).
//   __builtin_return_address(0) is the game caller: the entry patch is a jmp
//   (C-HOOK-1). SaveGame and Book get no generic detour.
//
// ONE PATCH PER TARGET (C-HOOK-5)
//   InstallHook copies whatever bytes it finds, so a second hook on a target would
//   chain into the first. slice-core owns the Add and factory targets; several areas
//   register handlers behind one detour. SliceRegisterHook covers other targets and
//   refuses anything owned here, patched by our other libraries, or overlapping their patch bytes (C-HOOK-2, C-HOOK-8).
//
// INSTALL ORDER (C-HOOK-4)
//   1. Connection() 0x3190240: its 51 bytes must verify, or Add is not hooked
//      (skipping Add constructs the result with it, C-ADD-10).
//   2. Add, when a handler can arm, observes Add, or a hook needs the cancel point.
//   3. The factories that have handlers. Arming stays refused unless 1 and 2 are in
//      and the guarded readers work, so no factory can promise ARMED 1 without a
//      working cancel point (the Windows double-apply).
//   4. The areas' own hooks.
//
// THE CANCEL (C-ADD-5..10, C-PTR-1..5)
//   A decoder arms from a factory handler: {Command* = the factory's rdi, the
//   factory's tag, the done policy}, only at a call site whose Command reaches Add
//   with no call in between (C-PTR-5). The arm is thread_local and the first Add on
//   that thread consumes it. The same pointer carrying the factory's tag skips Add:
//   done is fired or not per the policy, *ret is built by the game's own
//   Connection() (never written as 0: 0x31902c0 and 0x31904b0 dereference it), and
//   Add's own return value (ret) is returned. Any other pointer runs Add and drops
//   the arm. An arm still pending when the next top-level factory call starts is
//   stale (no armable window calls anything, and no factory calls a factory) and is
//   dropped. Every outcome reaches the decoder's `landed` callback.
#include "slice_core_internal.h"
#include "hook.h"
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

const uint8_t kSliceAddPrologue[14] = {
    0xf3, 0x0f, 0x1e, 0xfa,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xe5,              // mov  rbp, rsp
    0x41, 0x57,                    // push r15
    0x41, 0x56,                    // push r14
    0x41, 0x55,                    // push r13
};
// Connection(): new {0, 0}; the e8 is PC-relative (operator new, PLT 0x6dbce0), so
// the file bytes are the live bytes.
const uint8_t kSliceConnectionCtorBytes[51] = {
    0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x89, 0xfb, 0xbf, 0x10, 0x00, 0x00, 0x00,
    0x48, 0x83, 0xec, 0x08, 0xe8, 0x86, 0xba, 0x54, 0xfd, 0x48, 0xc7, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0x40, 0x08, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x03, 0x48, 0x83, 0xc4, 0x08, 0x5b,
    0x5d, 0xc3,
};

// Sites our other libraries patch (C-HOOK-2, C-HOOK-8), with the byte counts they
// verify or write. The plugin host patches only what a plugin asks for at run time,
// so its sites cannot be listed here.
static const struct { uintptr_t rva; size_t len; const char* owner; } kForeignPatches[] = {
    { 0x15ed140, 15, "tpf2_menu: typed native save factory hook (stack arguments)" },
    { 0x1019f40, 14, "tpf2_menu: native save completion observer" },
    { 0x1d86ff0, 15, "tpf2_menu: native input hold legacy script-event gate" },
    { 0xc0dc30,  14, "tpf2_bridge_mp: CGameTime::GetSpeed hook" },
    { 0xa31c30,  15, "tpf2_bridge_mp: CGame::Step hook" },
    { 0x1dc51d4, 44, "tpf2_bridge_mp: setPlayer patch A and its cave" },
    { 0x1dc5290,  9, "tpf2_bridge_mp: setPlayer patch B" },
    { 0x1dc4f8f, 14, "tpf2_bridge_mp: entity-only setPlayer dispatch" },
    { 0x113ca70, 17, "tpf2_menu: main page builder hook" },
    { 0x30de940, 16, "tpf2_menu: list-add hook" },
    { 0x1154b20, 16, "tpf2_menu: UI::CMenuUI::CreatePage hook" },
    { 0x35096e9,  5, "tpf2_menu: vk::DispatchLoaderDynamic::init call redirect" },
    { 0x30822e6,  5, "tpf2_menu_game: frame-loop Step call redirect" },
    { 0x312afb2,  5, "tpf2_menu_game: Step(c, 0, 0) call redirect" },
    { 0x112e948,  5, "tpf2_menu_game: step-listener Step call redirect" },
};

enum : int { kPhaseIdle = 0, kPhaseRegistering = 1, kPhaseClosed = 2, kPhaseFrozen = 3 };
static std::atomic<int> g_phase{kPhaseIdle};
static pthread_mutex_t g_regMu = PTHREAD_MUTEX_INITIALIZER;

static const int kFactorySlots = 37;
static const int kMaxHandlers = 8;
static const int kMaxObservers = 16;
static const int kMaxHooks = 32;
static const int kMaxAreas = 64;

struct FactorySlot {
    const SliceFactoryInfo* info;
    SliceFactoryHandler handlers[kMaxHandlers];
    int nHandlers;
    bool anyArms, anyReturn;
    void* tramp;                    // published by InstallHook before the patch
    bool installed;
    std::atomic<bool> seen;
    std::atomic<uint64_t> hits;
};
static FactorySlot g_slots[kFactorySlots];

struct AddObserver { const char* area; SliceAddObserverFn fn; void* ctx; };
static AddObserver g_observers[kMaxObservers];
static int g_nObservers = 0;

struct HookEntry { SliceHookSpec spec; std::atomic<bool> installed{false}; };
static HookEntry g_hooks[kMaxHooks];
static int g_nHooks = 0;

static void* g_addTramp = nullptr;
static std::atomic<bool> g_addInstalled{false};
static std::atomic<bool> g_connectionOk{false};

static std::atomic<uint64_t> g_addHits{0}, g_armed{0}, g_cancelFired{0}, g_cancelNotFired{0}, g_ranNative{0},
                             g_blocked{0}, g_mismatch{0}, g_superseded{0}, g_depthResets{0}, g_sinkAdds[3];
static std::atomic<bool> g_addSeen{false}, g_fireSeen{false}, g_sinkSeen[3];

// Per thread, all trivially destructible:
//   t_depth       > 0 while slice-core runs handler or decoder code on this thread;
//                 hooked functions reached meanwhile pass straight through.
//   t_depthFrame  the frame of the function that raised t_depth from 0. A later
//                 hooked call whose own frame is not below it proves that frame is
//                 gone (an exception unwound it): a live frame lies above everything
//                 it calls. A dead frame below the new call goes unnoticed until a
//                 shallower hooked call.
//   t_current     the factory call a handler is running for, as slice-core saw it:
//                 arming takes the slot, factory and caller from here, never from the
//                 handler-visible struct.
//   t_arm         the pending arm.
struct ArmState {
    bool active;
    void* cmd;
    int tag;
    const char* factory;
    uintptr_t factoryRet;
    SliceArm arm;
};
struct CurrentCall {
    const SliceFactoryCall* call;
    const SliceFactoryInfo* factory;
    uint64_t rdi;
    uintptr_t retRva;
    bool script, armable;
};
static thread_local int t_depth = 0;
static thread_local uintptr_t t_depthFrame = 0;
static thread_local CurrentCall t_current = {};
static thread_local ArmState t_arm = {};
static thread_local int t_tid = 0;
static thread_local const SliceAddCall* t_observedAdd = nullptr;
static thread_local void* t_overrideDone = nullptr;
static thread_local void (*t_overrideAfter)(void*, void*) = nullptr;
static thread_local void* t_overrideCtx = nullptr;

bool SliceOverrideAddDone(const SliceAddCall& add, void* done, void (*after)(void*, void*), void* ctx)
{
    uintptr_t manager = 0, invoker = 0;
    const uintptr_t address = (uintptr_t)done;
    if (t_observedAdd != &add || t_overrideDone || t_arm.active || !done ||
        address > UINTPTR_MAX - 32 || !SliceReadable(address,32) ||
        !SliceStdFunctionParts(address, &manager, &invoker) ||
        !manager || !invoker || !SliceInGameCode(manager) || !SliceInGameCode(invoker)) return false;
    t_overrideDone = done;
    t_overrideAfter = after;
    t_overrideCtx = ctx;
    return true;
}

#define SLICE_DEPTH_ENTER() do { if (t_depth++ == 0) t_depthFrame = (uintptr_t)__builtin_frame_address(0); } while (0)
#define SLICE_DEPTH_LEAVE() (void)(--t_depth)

static int Tid()
{
    if (!t_tid) t_tid = SliceTid();
    return t_tid;
}

const char* SliceOutcomeName(SliceOutcome o)
{
    switch (o) {
        case SliceOutcome::CancelledFired:    return "cancelled, done fired";
        case SliceOutcome::CancelledNotFired: return "cancelled, done not fired";
        case SliceOutcome::Blocked:           return "blocked: strict capture unavailable";
        case SliceOutcome::RanNatively:       return "ran natively";
        case SliceOutcome::Mismatch:          return "dropped: another command reached Add";
        case SliceOutcome::Superseded:        return "dropped: re-armed or stale before any Add";
    }
    return "?";
}

// ---- the cancel ---------------------------------------------------------------------------

bool SliceCancelAvailable()
{
    return g_addInstalled.load(std::memory_order_relaxed) && g_connectionOk.load(std::memory_order_relaxed) &&
           SliceReadsOk();
}

static void Landed(const ArmState& st, const SliceAddCall* add, SliceOutcome o)
{
    switch (o) {
        case SliceOutcome::CancelledFired:    g_cancelFired++; break;
        case SliceOutcome::CancelledNotFired: g_cancelNotFired++; break;
        case SliceOutcome::Blocked:           g_blocked++; break;
        case SliceOutcome::RanNatively:       g_ranNative++; break;
        case SliceOutcome::Mismatch:          g_mismatch++; break;
        case SliceOutcome::Superseded:        g_superseded++; break;
    }
    if (!st.arm.landed) return;
    SLICE_DEPTH_ENTER();
    st.arm.landed(add, o, st.arm.ctx);
    SLICE_DEPTH_LEAVE();
}

static void DropStaleArm(const char* why)
{
    if (!t_arm.active) return;
    const ArmState old = t_arm;
    t_arm.active = false;
    SliceLog("[slice] armed %s (cmd=%p, %s caller_rva=%lx) dropped on thread %d before any Add: %s\n",
             old.arm.what ? old.arm.what : "?", old.cmd, old.factory ? old.factory : "?", (unsigned long)old.factoryRet,
             Tid(), why);
    Landed(old, nullptr, SliceOutcome::Superseded);
}

// t_depth is raised but the frame that raised it is gone: an exception unwound a
// handler (or a decoder callback) past slice-core's bookkeeping.
__attribute__((noinline))
static void ResetStuckDepth(const char* where)
{
    const int depth = t_depth;
    t_depth = 0;
    t_depthFrame = 0;
    t_current = {};
    g_depthResets++;
    SliceLog("[slice] %s on thread %d: handler depth %d left by a frame that is gone (an exception unwound a "
             "handler) -- reset; hooks on this thread were bypassed until now\n", where, Tid(), depth);
    DropStaleArm("its handler's stack was unwound");
}

bool SliceArmCancel(const SliceFactoryCall& call, const SliceArm& arm)
{
    const char* what = arm.what ? arm.what : "?";
    const CurrentCall cur = t_current;
    const bool own = cur.call && &call == cur.call;
    const char* refuse = nullptr;
    if (!own) refuse = "not called from this thread's factory handler for this call";
    else if (cur.script) refuse = "a script caller: a Lua replay or a mod's command is never cancelled";
    else if (!cur.armable) refuse = "not an armable call site: this Command is not known to reach Add next (C-PTR-5)";
    else if (!cur.rdi) refuse = "no Command slot";
    else if (!SliceCancelAvailable()) refuse = "the cancel point is off (Add not hooked or guarded reads unavailable)";
    if (refuse) {
        SliceLog("[slice] arm refused for %s (%s, caller_rva=%lx): %s\n", what, own && cur.factory ? cur.factory->name : "?",
                 (unsigned long)(own ? cur.retRva : call.retRva), refuse);
        return false;
    }
    if (t_arm.active) {
        const ArmState old = t_arm;
        t_arm.active = false;
        SliceLog("[slice] armed %s (cmd=%p) superseded on thread %d before any Add -- dropped\n",
                 old.arm.what ? old.arm.what : "?", old.cmd, Tid());
        Landed(old, nullptr, SliceOutcome::Superseded);
    }
    ArmState st = {};
    st.active = true;
    st.cmd = (void*)cur.rdi;
    st.tag = cur.factory->tag;
    st.factory = cur.factory->name;
    st.factoryRet = cur.retRva;
    st.arm = arm;
    t_arm = st;
    g_armed++;
    SliceLog("[slice] armed cancel: %s cmd=%p (%s tag 0x%x, caller_rva=%lx, thread %d, done %s%s)\n", what, st.cmd,
             st.factory, st.tag, (unsigned long)cur.retRva, Tid(),
             arm.done == SliceDone::Never ? "never fired" : arm.done == SliceDone::IfPresent ? "fired if present" : "required",
             arm.honourArmed ? ", honoured when not fired" : "");
    return true;
}

void SliceDisarm(const SliceFactoryCall& call)
{
    const uint64_t slot = t_current.call == &call ? t_current.rdi : call.rdi;
    if (!t_arm.active || t_arm.cmd != (void*)slot) return;
    t_arm.active = false;
    SliceLog("[slice] disarmed %s cmd=%p on thread %d\n", t_arm.arm.what ? t_arm.arm.what : "?", t_arm.cmd, Tid());
}

bool SliceShipAndArm(const SliceFactoryCall& call, const SliceArm& arm, const SliceRecord& rec)
{
    const char* what = arm.what ? arm.what : "?";
    const CurrentCall cur = t_current;
    const bool own = cur.call && &call == cur.call;
    if (own && arm.done == SliceDone::Required && cur.factory &&
        (cur.factory->tag == 0x0f || cur.factory->tag == 0x11))
        SliceLog("[slice] %s: SliceShipAndArm with done Required on %s -- a tool that waits on its callback is wedged "
                 "when it cannot be fired; arm with SliceArmCancel and write the record from landed\n", what,
                 cur.factory->name);
    // Multiplayer actions always cancel and replay. A capture/arming failure
    // leaves no ARMED 0 record; the central Add barrier blocks the player click.
    if (!SliceSessionLive()) return false;
    if (!SliceCancelAvailable() || !own || cur.script || !cur.armable) return false;
    SliceArm a = arm;
    a.honourArmed = true;
    if (!SliceArmCancel(call, a)) return false;
    const auto written = SliceInjectWrite(rec, SliceArmedLine::One);
    if (written != SliceInjectResult::NotWritten) return true;
    SliceDisarm(call);
    SliceLog("[slice] %s: record not written -- strict Add barrier will block the action\n", what);
    return false;
}

using AddFn = void* (*)(void* ret, void* list, void* cmd, void* done, void* progress);

// No destructors or catch here: game exceptions pass through untouched. The
// override owner retains its storage if the original Add throws.
static void* CallOriginalAdd(AddFn orig, void* ret, void* list, void* cmd, void* done, void* progress,
                             void (*after)(void*, void*), void* ctx)
{
    void* result = orig(ret, list, cmd, done, progress);
    if (after) after(done, ctx);
    return result;
}


// A failed capture never executes locally. Only decoder-owned, verified
// cleanup in Landed(Blocked) may release its tool state.
static void* BlockAdd(const SliceAddCall& a, const ArmState* st, const char* reason)
{
    // Only the decoder can release tool state safely on failure. CreateLine
    // callbacks assert without a result, and Clock callbacks run before their
    // caller increments its counter. Game .text membership proves neither.
    SliceLog("[slice] BLOCKED command tag=%d add_rva=%lx: %s (strict cancel/replay)\n",
             SliceCommandTag((uintptr_t)a.cmd), (unsigned long)a.retRva, reason);
    ((void (*)(void*))(SliceImageBase() + kSliceRvaConnectionCtor))(a.ret);
    if (st) Landed(*st, &a, SliceOutcome::Blocked);
    else g_blocked++;
    return a.ret;
}

static bool MustBlockUncaptured(const SliceAddCall& a)
{
    return SlicePlayerAddSite(a.retRva) && SliceSessionLive();
}

static void* AddDispatch(void* ret, void* list, void* cmd, void* done, void* progress, uintptr_t retAddr)
{
    const AddFn orig = (AddFn)g_addTramp;
    g_addHits.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t retRva = SliceRvaOf(retAddr);

    // What the first live run has to answer (SLICE_CORE.md section 11): which
    // threads call Add, and whether the mod's commands reach Add through a script sink.
    if (!g_addSeen.load(std::memory_order_relaxed) && !g_addSeen.exchange(true))
        SliceLog("[slice] first CommandList::Add on thread %d from %lx (%s)\n", Tid(), (unsigned long)retRva,
                 SliceAddCallerKind(retRva));
    const int sink = retRva == 0xa2f5c2 ? 0 : retRva == 0x11225a9 ? 1 : retRva == 0x1d873b2 ? 2 : -1;
    if (sink >= 0) {
        g_sinkAdds[sink].fetch_add(1, std::memory_order_relaxed);
        if (!g_sinkSeen[sink].load(std::memory_order_relaxed) && !g_sinkSeen[sink].exchange(true))
            SliceLog("[slice] first Add from script sink %lx on thread %d\n", (unsigned long)retRva, Tid());
    }

    if (t_depth > 0) {
        if ((uintptr_t)__builtin_frame_address(0) < t_depthFrame) return orig(ret, list, cmd, done, progress);
        ResetStuckDepth("CommandList::Add");
    }

    const SliceAddCall a = { ret, list, cmd, done, progress, retAddr, retRva, Tid() };
    void (*overrideAfter)(void*, void*) = nullptr;
    void* overrideCtx = nullptr;
    if (g_nObservers) {
        t_observedAdd = &a;
        t_overrideDone = nullptr;
        t_overrideAfter = nullptr;
        t_overrideCtx = nullptr;
        SLICE_DEPTH_ENTER();
        for (int i = 0; i < g_nObservers; i++) g_observers[i].fn(a, g_observers[i].ctx);
        SLICE_DEPTH_LEAVE();
        if (t_overrideDone) done = t_overrideDone;
        overrideAfter = t_overrideAfter;
        overrideCtx = t_overrideCtx;
        t_observedAdd = nullptr;
        t_overrideDone = nullptr;
    }
    if (!t_arm.active) {
        if (MustBlockUncaptured(a)) return BlockAdd(a, nullptr, "player action has no committed replay capture");
        return CallOriginalAdd(orig, ret, list, cmd, done, progress, overrideAfter, overrideCtx);
    }

    const ArmState st = t_arm;
    t_arm.active = false;   // consumed by this Add, whatever happens next (C-PTR-4)
    const char* what = st.arm.what ? st.arm.what : "?";
    if (cmd != st.cmd) {
        SliceLog("[slice] armed %s (cmd=%p) dropped: the next Add on thread %d carried cmd=%p from %lx (%s) -- it runs\n",
                 what, st.cmd, a.tid, cmd, (unsigned long)retRva, SliceAddCallerKind(retRva));
        Landed(st, &a, SliceOutcome::Mismatch);
        if (MustBlockUncaptured(a)) return BlockAdd(a, nullptr, "capture command pointer mismatch");
        return CallOriginalAdd(orig, ret, list, cmd, done, progress, overrideAfter, overrideCtx);
    }

    const char* refuse = nullptr;
    const int tag = SliceCommandTag((uintptr_t)cmd);
    if (!SliceCancelAvailable()) refuse = "the cancel point is off";
    else if (tag != st.tag) refuse = tag < 0 ? "the Command's tag is unreadable" : "the Command's tag is not the factory's";

    uintptr_t invoker = 0;
    bool fire = false;
    if (!refuse && st.arm.done != SliceDone::Never) {
        uintptr_t manager = 0;
        const char* noFire = nullptr;
        bool empty = false;
        if (!done || !SliceStdFunctionParts((uintptr_t)done, &manager, &invoker)) noFire = "done is unreadable";
        else if (!manager) { empty = true; noFire = "done is empty"; }
        else if (!invoker || !SliceInGameCode(invoker)) noFire = "done's invoker is not in the game's .text";
        if (!noFire) fire = true;
        else if (empty && st.arm.done == SliceDone::IfPresent) {}
        else if (st.arm.honourArmed) {
            SliceLog("[slice] %s: %s but cancel is ARMED -- honouring it (caller_rva=%lx)\n",
                     what, noFire, (unsigned long)retRva);
        } else refuse = noFire;
    }
    if (!refuse && st.arm.prepareCancel) {
        SLICE_DEPTH_ENTER();
        const bool prepared = st.arm.prepareCancel(a, st.arm.ctx);
        SLICE_DEPTH_LEAVE();
        if (!prepared) refuse = "capture could not commit";
    }
    if (refuse) {
        if (SliceSessionLive() || st.arm.honourArmed) return BlockAdd(a, &st, refuse);
        SliceLog("[slice] armed %s NOT cancelled (%s; tag %d, expected 0x%x) -- Add runs natively (caller_rva=%lx)\n",
                 what, refuse, tag, st.tag, (unsigned long)retRva);
        Landed(st, &a, SliceOutcome::RanNatively);
        return CallOriginalAdd(orig, ret, list, cmd, done, progress, overrideAfter, overrideCtx);
    }

    bool fired = false;
    if (fire) {
        if (st.arm.beforeFire) {
            SLICE_DEPTH_ENTER();
            st.arm.beforeFire(a, st.arm.ctx);
            SLICE_DEPTH_LEAVE();
        }
        if (!g_fireSeen.load(std::memory_order_relaxed) && !g_fireSeen.exchange(true))
            SliceLog("[slice] first done fired from Add on thread %d (invoker %lx)\n",
                     a.tid, (unsigned long)SliceRvaOf(invoker));
        ((void (*)(void*, void*))invoker)(done, cmd);
        fired = true;
    }

    // Skip Add the way its own empty-done path returns (C-ADD-10): *ret = Connection().
    ((void (*)(void*))(SliceImageBase() + kSliceRvaConnectionCtor))(ret);
    SliceLog("[slice] CANCEL %s (%s, caller_rva=%lx, add_rva=%lx, thread %d), done %s -- now owned by lockstep\n", what,
             st.factory, (unsigned long)st.factoryRet, (unsigned long)retRva, a.tid, fired ? "fired" : "not fired");
    Landed(st, &a, fired ? SliceOutcome::CancelledFired : SliceOutcome::CancelledNotFired);
    return ret;
}

static void* AddDetour(void* ret, void* list, void* cmd, void* done, void* progress)
{
    return AddDispatch(ret, list, cmd, done, progress, (uintptr_t)__builtin_return_address(0));
}

// ---- factories -------------------------------------------------------------------------------

using FactoryFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                               __m128, __m128, __m128, __m128, __m128, __m128, __m128, __m128);

__attribute__((noinline))
static uint64_t FactoryDispatch(int slot, uintptr_t retAddr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, __m128 x0, __m128 x1, __m128 x2, __m128 x3, __m128 x4,
                                __m128 x5, __m128 x6, __m128 x7)
{
    FactorySlot& s = g_slots[slot];
    const FactoryFn orig = (FactoryFn)s.tramp;
    s.hits.fetch_add(1, std::memory_order_relaxed);
    if (t_depth > 0) {
        if ((uintptr_t)__builtin_frame_address(0) < t_depthFrame)
            return orig(a0, a1, a2, a3, a4, a5, x0, x1, x2, x3, x4, x5, x6, x7);
        ResetStuckDepth(s.info->name);
    }
    // A top-level factory call with an arm pending: that arm is stale. Between an
    // armable factory call and its Add nothing is called (C-PTR-5) and no factory
    // calls a factory (C-FAC-4), so its Command will never reach Add -- its factory
    // threw after an onEntry arm. Left armed it could cancel a later Command that
    // reuses the same stack slot.
    DropStaleArm("a new factory call started on the thread");

    SliceFactoryCall c = {};
    c.factory = s.info;
    c.rdi = a0; c.rsi = a1; c.rdx = a2; c.rcx = a3; c.r8 = a4; c.r9 = a5;
    c.xmm[0] = x0; c.xmm[1] = x1; c.xmm[2] = x2; c.xmm[3] = x3;
    c.xmm[4] = x4; c.xmm[5] = x5; c.xmm[6] = x6; c.xmm[7] = x7;
    c.retAddr = retAddr;
    c.retRva = SliceRvaOf(retAddr);
    c.script = SliceIsScriptCaller(c.retRva);
    c.armable = !c.script && SliceIsArmableSite(c.retRva, s.info->rva);
    c.tid = Tid();
    const CurrentCall cur = { &c, s.info, a0, c.retRva, c.script, c.armable };
    if (!s.seen.load(std::memory_order_relaxed) && !s.seen.exchange(true))
        SliceLog("[slice] first call of %s on thread %d from %lx%s\n", s.info->name, c.tid, (unsigned long)c.retRva,
                 c.script ? " (script)" : c.armable ? "" : " (not armable)");

    SLICE_DEPTH_ENTER();
    t_current = cur;
    for (int i = 0; i < s.nHandlers; i++)
        if (s.handlers[i].onEntry) s.handlers[i].onEntry(c, s.handlers[i].ctx);
    t_current = {};
    SLICE_DEPTH_LEAVE();

    // The registers as the game passed them, whatever a handler did.
    const uint64_t rax = orig(a0, a1, a2, a3, a4, a5, x0, x1, x2, x3, x4, x5, x6, x7);

    if (s.anyReturn) {
        c.rax = rax;
        SLICE_DEPTH_ENTER();
        t_current = cur;
        for (int i = 0; i < s.nHandlers; i++)
            if (s.handlers[i].onReturn) s.handlers[i].onReturn(c, s.handlers[i].ctx);
        t_current = {};
        SLICE_DEPTH_LEAVE();
    }
    return rax;
}

template <int N>
static uint64_t FactoryDetour(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                              __m128 x0, __m128 x1, __m128 x2, __m128 x3, __m128 x4, __m128 x5, __m128 x6, __m128 x7)
{
    return FactoryDispatch(N, (uintptr_t)__builtin_return_address(0), a0, a1, a2, a3, a4, a5, x0, x1, x2, x3, x4, x5,
                           x6, x7);
}

static const FactoryFn kFactoryDetours[kFactorySlots] = {
    &FactoryDetour<0>,  &FactoryDetour<1>,  &FactoryDetour<2>,  &FactoryDetour<3>,  &FactoryDetour<4>,
    &FactoryDetour<5>,  &FactoryDetour<6>,  &FactoryDetour<7>,  &FactoryDetour<8>,  &FactoryDetour<9>,
    &FactoryDetour<10>, &FactoryDetour<11>, &FactoryDetour<12>, &FactoryDetour<13>, &FactoryDetour<14>,
    &FactoryDetour<15>, &FactoryDetour<16>, &FactoryDetour<17>, &FactoryDetour<18>, &FactoryDetour<19>,
    &FactoryDetour<20>, &FactoryDetour<21>, &FactoryDetour<22>, &FactoryDetour<23>, &FactoryDetour<24>,
    &FactoryDetour<25>, &FactoryDetour<26>, &FactoryDetour<27>, &FactoryDetour<28>, &FactoryDetour<29>,
    &FactoryDetour<30>, &FactoryDetour<31>, &FactoryDetour<32>, &FactoryDetour<33>, &FactoryDetour<34>,
    &FactoryDetour<35>, &FactoryDetour<36>,
};

// ---- registration ------------------------------------------------------------------------------

static bool Overlaps(uintptr_t a, size_t alen, uintptr_t b, size_t blen) { return a < b + blen && b < a + alen; }

// Check the RVA before comparing ownership ranges: base+rva wrapping must not
// make an out-of-image registration alias an already-owned factory or Add.
// Expected bytes must lie within one executable mapping, without crossing a gap.
static bool HookImageRange(uintptr_t rva, size_t bytes)
{
    const uintptr_t base=SliceImageBase();
    if (!base || !bytes || rva > UINTPTR_MAX-base) return false;
    const uintptr_t address=base+rva;
    if (bytes-1 > UINTPTR_MAX-address || SliceRvaOf(address) != rva) return false;
    const SliceCoreEnv& env=SliceCoreEnvGet();
    for (int i=0;i<env.nExec;++i)
        if (address >= env.exec[i].lo && address < env.exec[i].hi && bytes <= env.exec[i].hi-address) return true;
    return false;
}

// All shipped patchers use Tpf2mpCodeWriteSelf without changing permissions.
// Separate sites on one RX page are safe; overlapping bytes remain forbidden.
static const char* ForeignPatchOwner(uintptr_t rva, size_t len)
{
    for (const auto& fp : kForeignPatches)
        if (Overlaps(rva, len, fp.rva, fp.len)) return fp.owner;
    return nullptr;
}

static bool Registering(const char* area, const char* what)
{
    if (g_phase.load() == kPhaseRegistering) return true;
    SliceLog("[slice] %s: %s refused -- registration is only open inside SLICE_AREA\n", area ? area : "?", what);
    return false;
}

// A refused registration must not disappear from the readiness decision just
// because it never became an install candidate. Protected by g_regMu.
static int g_registrationFailures = 0;

void SliceCoreBeginRegistration()
{
    pthread_mutex_lock(&g_regMu);
    size_t n = 0;
    const SliceFactoryInfo* table = SliceFactories(&n);
    for (int i = 0; i < kFactorySlots && (size_t)i < n; i++) g_slots[i].info = &table[i];
    if (g_phase.load() == kPhaseIdle) g_phase.store(kPhaseRegistering);
    pthread_mutex_unlock(&g_regMu);
}

void SliceCoreEndRegistration()
{
    pthread_mutex_lock(&g_regMu);
    if (g_phase.load() == kPhaseRegistering) g_phase.store(kPhaseClosed);
    pthread_mutex_unlock(&g_regMu);
}

bool SliceOnFactory(const SliceFactoryHandler& h)
{
    const char* area = h.area ? h.area : "?";
    pthread_mutex_lock(&g_regMu);
    const char* refuse = nullptr;
    const SliceFactoryInfo* f = SliceFactoryByRva(h.factoryRva);
    size_t n = 0;
    const SliceFactoryInfo* table = SliceFactories(&n);
    const int idx = f ? (int)(f - table) : -1;
    if (!Registering(area, "a factory handler")) refuse = "closed";
    else if (!f || idx < 0 || idx >= kFactorySlots) refuse = "not a factory RVA of build 35924";
    else if (f->stackArgs) refuse = "the factory reads stack arguments; the generic detour cannot pass them on (C-FAC-4)";
    else if (!h.onEntry && !h.onReturn) refuse = "no handler function";
    else if (g_slots[idx].nHandlers >= kMaxHandlers) refuse = "too many handlers on this factory";
    if (refuse) {
        ++g_registrationFailures;
        if (strcmp(refuse, "closed") != 0)
            SliceLog("[slice] %s: handler on %lx refused: %s\n", area, (unsigned long)h.factoryRva, refuse);
        pthread_mutex_unlock(&g_regMu);
        return false;
    }
    FactorySlot& s = g_slots[idx];
    int at = s.nHandlers++;                        // stable: after every handler of the same or lower order
    while (at > 0 && s.handlers[at - 1].order > h.order) { s.handlers[at] = s.handlers[at - 1]; at--; }
    s.handlers[at] = h;
    s.anyArms = s.anyArms || h.arms;
    s.anyReturn = s.anyReturn || h.onReturn;
    SliceLog("[slice] %s: handler on %s (%lx), order %d%s%s%s\n", area, f->name, (unsigned long)f->rva, h.order,
             h.onEntry ? " entry" : "", h.onReturn ? " return" : "", h.arms ? ", may arm a cancel" : "");
    pthread_mutex_unlock(&g_regMu);
    return true;
}

bool SliceOnAdd(const char* area, SliceAddObserverFn fn, void* ctx)
{
    pthread_mutex_lock(&g_regMu);
    bool ok = Registering(area, "an Add observer") && fn && g_nObservers < kMaxObservers;
    if (ok) {
        g_observers[g_nObservers++] = { area ? area : "?", fn, ctx };
        SliceLog("[slice] %s: observer on CommandList::Add\n", area ? area : "?");
    } else if (g_phase.load() == kPhaseRegistering) {
        SliceLog("[slice] %s: Add observer refused (%s)\n", area ? area : "?", fn ? "too many" : "no function");
    }
    if (!ok) ++g_registrationFailures;
    pthread_mutex_unlock(&g_regMu);
    return ok;
}

bool SliceRegisterHook(const SliceHookSpec& s)
{
    const char* area = s.area ? s.area : "?";
    const char* name = s.name ? s.name : "?";
    pthread_mutex_lock(&g_regMu);
    char why[256] = "";
    uint8_t expectedPrologue[64]{};
    if (!Registering(area, name)) {
        ++g_registrationFailures;
        pthread_mutex_unlock(&g_regMu);
        return false;
    }
    if (!s.detour || !s.trampoline || !s.expected) snprintf(why, sizeof(why), "missing detour, trampoline slot or expected bytes");
    else if (s.steal < 5 || s.steal > 32 || s.expectedLen < (size_t)s.steal)
        snprintf(why, sizeof(why), "steal %d must be 5..32 and covered by the expected bytes (%zu)", s.steal, s.expectedLen);
    else if (!HookImageRange(s.rva,s.expectedLen))
        snprintf(why,sizeof(why),"RVA or expected-byte span is outside one executable image mapping");
    else if ((uintptr_t)s.expected > UINTPTR_MAX-s.expectedLen ||
             !SliceRead((uintptr_t)s.expected,expectedPrologue,s.expectedLen < sizeof(expectedPrologue) ? s.expectedLen : sizeof(expectedPrologue)))
        snprintf(why,sizeof(why),"expected bytes are unreadable");
    else if (PrologueSteal(expectedPrologue, s.steal) != s.steal)
        snprintf(why, sizeof(why), "steal %d is not an instruction boundary the prologue decoder accepts", s.steal);
    else if (g_nHooks >= kMaxHooks) snprintf(why, sizeof(why), "too many hooks");
    if (!why[0]) {
        size_t n = 0;
        const SliceFactoryInfo* table = SliceFactories(&n);
        for (size_t i = 0; i < n && !why[0]; i++)
            if (Overlaps(s.rva, (size_t)s.steal, table[i].rva, table[i].steal))
                snprintf(why, sizeof(why), "overlaps factory %s, owned by slice-core (use SliceOnFactory)", table[i].name);
        if (!why[0] && Overlaps(s.rva, (size_t)s.steal, kSliceRvaAdd, kSliceStealAdd))
            snprintf(why, sizeof(why), "overlaps CommandList::Add, owned by slice-core (use SliceOnAdd)");
        if (!why[0] && Overlaps(s.rva, (size_t)s.steal, kSliceRvaConnectionCtor, sizeof(kSliceConnectionCtorBytes)))
            snprintf(why, sizeof(why), "overlaps Connection(), which the cancel calls");
        if (!why[0]) {
            if (const char* owner = ForeignPatchOwner(s.rva, (size_t)s.steal))
                snprintf(why, sizeof(why), "overlaps a site patched by %s", owner);
        }
        for (int i = 0; i < g_nHooks && !why[0]; i++)
            if (Overlaps(s.rva, (size_t)s.steal, g_hooks[i].spec.rva, (size_t)g_hooks[i].spec.steal))
                snprintf(why, sizeof(why), "overlaps %s of %s", g_hooks[i].spec.name, g_hooks[i].spec.area);
    }
    if (why[0]) {
        ++g_registrationFailures;
        SliceLog("[slice] %s: hook %s (%lx) refused: %s\n", area, name, (unsigned long)s.rva, why);
        pthread_mutex_unlock(&g_regMu);
        return false;
    }
    g_hooks[g_nHooks].spec=s;
    g_hooks[g_nHooks].installed.store(false,std::memory_order_relaxed);
    ++g_nHooks;
    SliceLog("[slice] %s: hook %s (%lx, steal %d)%s\n", area, name, (unsigned long)s.rva, s.steal,
             s.needsCancel ? ", needs the cancel point" : "");
    pthread_mutex_unlock(&g_regMu);
    return true;
}

bool SliceHookInstalled(uintptr_t rva)
{
    // Registration is immutable once Frozen is published. The installed bits
    // remain independent because each patch may succeed or fail afterwards.
    if (g_phase.load(std::memory_order_acquire) != kPhaseFrozen) return false;
    for (int i=0;i<g_nHooks;++i)
        if (g_hooks[i].spec.rva==rva) return g_hooks[i].installed.load(std::memory_order_acquire);
    return false;
}

// slice-core's own entry keeps the section present, so the linker always defines
// its bounds (a hidden reference to an undefined __start_ symbol cannot link into
// a shared object).
SLICE_AREA(slice_core_self, "slice-core") {}

extern const SliceAreaEntry __start_tpf2mp_slice_areas[] __attribute__((visibility("hidden")));
extern const SliceAreaEntry __stop_tpf2mp_slice_areas[] __attribute__((visibility("hidden")));

int SliceCoreRunRegistrations()
{
    const SliceAreaEntry* b = __start_tpf2mp_slice_areas;
    const SliceAreaEntry* e = __stop_tpf2mp_slice_areas;
    const size_t count = (size_t)(e - b);
    if (count > (size_t)kMaxAreas) {
        // Registering some would leave the rest's captures running natively without a
        // word; refusing all leaves the game unpatched and says so.
        SliceLog("[slice] %zu SLICE_AREA entries, the registry holds %d -- no area registered, nothing patched\n",
                 count, kMaxAreas);
        return -1;
    }
    const SliceAreaEntry* order[kMaxAreas];
    int n = 0;
    for (const SliceAreaEntry* p = b; p < e; p++) {
        int j = n++;
        while (j > 0 && strcmp(order[j - 1]->name, p->name) > 0) { order[j] = order[j - 1]; j--; }
        order[j] = p;
    }
    SliceCoreBeginRegistration();
    for (int i = 0; i < n; i++) {
        SliceLog("[slice] area %s: registering\n", order[i]->name);
        order[i]->registerFn();
    }
    SliceCoreEndRegistration();
    return n;
}

// ---- install --------------------------------------------------------------------------------------

// The image's code is mapped by the loader; a target is compared only when the
// whole range lies in one of its executable segments.
static bool LiveBytesMatch(uintptr_t rva, const uint8_t* expected, size_t n)
{
    if (!expected || !HookImageRange(rva,n) || (uintptr_t)expected > UINTPTR_MAX-n) return false;
    const uintptr_t addr = SliceImageBase() + rva;
    uint8_t live[256], wanted[256];
    for (size_t offset=0;offset<n;) {
        const size_t len=n-offset < sizeof(live) ? n-offset : sizeof(live);
        if (!SliceRead(addr+offset,live,len) || !SliceRead((uintptr_t)expected+offset,wanted,len) || memcmp(live,wanted,len)) return false;
        offset+=len;
    }
    return true;
}

SliceInstallReport SliceCoreInstall()
{
    SliceInstallReport rep = {};
    pthread_mutex_lock(&g_regMu);
    const int phase = g_phase.load();
    const int registrationFailures = g_registrationFailures;
    g_phase.store(kPhaseFrozen);
    pthread_mutex_unlock(&g_regMu);
    if (phase == kPhaseFrozen) { SliceLog("[slice] install ran twice -- ignored\n"); return rep; }
    const uintptr_t base = SliceImageBase();
    if (!base) { SliceLog("[slice] no image base -- nothing installed\n"); return rep; }

    bool wantAdd = true; // strict player barrier is required even without a decoder
    int withHandlers = 0;
    for (int i = 0; i < kFactorySlots; i++) {
        if (!g_slots[i].nHandlers) continue;
        withHandlers++;
        wantAdd = wantAdd || g_slots[i].anyArms;
    }
    for (int i = 0; i < g_nHooks; i++) wantAdd = wantAdd || g_hooks[i].spec.needsCancel;

    // 1 + 2. Connection() and Add.
    rep.connectionOk = LiveBytesMatch(kSliceRvaConnectionCtor, kSliceConnectionCtorBytes, sizeof(kSliceConnectionCtorBytes));
    g_connectionOk.store(rep.connectionOk);
    const char* addPageOwner = ForeignPatchOwner(kSliceRvaAdd, kSliceStealAdd);
    if (!wantAdd) {
        SliceLog("[slice] no handler arms a cancel or observes Add -- CommandList::Add not hooked\n");
    } else if (!rep.connectionOk) {
        SliceLog("[slice] Connection() at 3190240 differs from build 35924 -- CommandList::Add NOT hooked, nothing can be cancelled\n");
    } else if (addPageOwner) {
        SliceLog("[slice] CommandList::Add overlaps a site patched by %s -- NOT hooked, nothing can be cancelled\n",
                 addPageOwner);
    } else if (!LiveBytesMatch(kSliceRvaAdd, kSliceAddPrologue, sizeof(kSliceAddPrologue)) ||
               PrologueSteal((const unsigned char*)(base + kSliceRvaAdd), 14) != kSliceStealAdd) {
        SliceLog("[slice] CommandList::Add prologue differs from build 35924 -- NOT hooked, nothing can be cancelled\n");
    } else if (!InstallHook(base + kSliceRvaAdd, (void*)&AddDetour, kSliceStealAdd, &g_addTramp)) {
        SliceLog("[slice] HOOK FAILED CommandList::Add rva=%lx -- nothing can be cancelled\n", (unsigned long)kSliceRvaAdd);
    } else {
        g_addInstalled.store(true);
        rep.addInstalled = true;
        SliceLog("[slice] hooked CommandList::Add rva=%lx steal=%d id=1 (%d observer(s))\n", (unsigned long)kSliceRvaAdd,
                 kSliceStealAdd, g_nObservers);
    }
    if (wantAdd)
        SliceLog("[slice] cancel point %s\n", SliceCancelAvailable() ? "available"
                 : rep.addInstalled ? "OFF: guarded reads unavailable (every arm is refused)" : "OFF (every arm is refused)");

    // 3. The factories that have handlers.
    for (int i = 0; i < kFactorySlots; i++) {
        FactorySlot& s = g_slots[i];
        if (!s.nHandlers) continue;
        const SliceFactoryInfo* f = s.info;
        if (const char* owner = ForeignPatchOwner(f->rva, f->steal)) {
            SliceLog("[slice] %s overlaps a site patched by %s -- NOT hooked (%d handler(s) idle)\n", f->name,
                     owner, s.nHandlers);
            rep.factoriesSkipped++;
            continue;
        }
        if (!LiveBytesMatch(f->rva, f->prologue, f->steal) ||
            PrologueSteal((const unsigned char*)(base + f->rva), 14) != f->steal) {
            SliceLog("[slice] %s prologue differs from build 35924 -- NOT hooked (%d handler(s) idle)\n", f->name, s.nHandlers);
            rep.factoriesSkipped++;
            continue;
        }
        if (!InstallHook(base + f->rva, (void*)kFactoryDetours[i], f->steal, &s.tramp)) {
            SliceLog("[slice] HOOK FAILED %s rva=%lx\n", f->name, (unsigned long)f->rva);
            rep.factoriesSkipped++;
            continue;
        }
        s.installed = true;
        rep.factoriesInstalled++;
        SliceLog("[slice] hooked %s rva=%lx steal=%d id=%d tag=0x%x, %d handler(s)%s\n", f->name, (unsigned long)f->rva,
                 f->steal, f->winId, f->tag, s.nHandlers,
                 s.anyArms && !SliceCancelAvailable() ? " -- arming OFF: no cancel point" : "");
    }

    // 4. The areas' own hooks.
    for (int i = 0; i < g_nHooks; i++) {
        HookEntry& h = g_hooks[i];
        const SliceHookSpec& sp = h.spec;
        if (sp.needsCancel && !SliceCancelAvailable()) {
            SliceLog("[slice] %s: %s NOT hooked -- it needs the cancel point, which is off\n", sp.area, sp.name);
            rep.hooksSkipped++;
            continue;
        }
        if (!LiveBytesMatch(sp.rva, sp.expected, sp.expectedLen) ||
            PrologueSteal((const unsigned char*)(base + sp.rva), sp.steal) != sp.steal) {
            SliceLog("[slice] %s: %s bytes differ from build 35924 -- NOT hooked\n", sp.area, sp.name);
            rep.hooksSkipped++;
            continue;
        }
        if (!InstallHook(base + sp.rva, sp.detour, sp.steal, sp.trampoline)) {
            SliceLog("[slice] %s: HOOK FAILED %s rva=%lx\n", sp.area, sp.name, (unsigned long)sp.rva);
            rep.hooksSkipped++;
            continue;
        }
        h.installed.store(true,std::memory_order_release);
        rep.hooksInstalled++;
        SliceLog("[slice] %s: hooked %s rva=%lx steal=%d id=%d\n", sp.area, sp.name, (unsigned long)sp.rva, sp.steal,
                 sp.winId);
    }
    SliceLog("[slice] install done: Add %s, %d of %d factories, %d of %d other hooks\n",
             rep.addInstalled ? "hooked" : "not hooked", rep.factoriesInstalled, withHandlers, rep.hooksInstalled, g_nHooks);
    rep.ready = phase == kPhaseClosed && !registrationFailures && SliceCancelAvailable() &&
                !rep.factoriesSkipped && !rep.hooksSkipped;
    if (registrationFailures)
        SliceLog("[slice] %d registration failure(s) -- multiplayer startup refused\n", registrationFailures);
    return rep;
}

void SliceCoreLogAlive()
{
    uint64_t factoryHits = 0;
    for (int i = 0; i < kFactorySlots; i++) factoryHits += g_slots[i].hits.load(std::memory_order_relaxed);
    SliceLog("[slice] alive: addHits=%llu armed=%llu cancelled=%llu (done fired %llu) ranNative=%llu blocked=%llu mismatch=%llu "
             "superseded=%llu depthResets=%llu scriptSinkAdds=%llu/%llu legacyScriptAdds=%llu factoryHits=%llu\n",
             (unsigned long long)g_addHits.load(), (unsigned long long)g_armed.load(),
             (unsigned long long)(g_cancelFired.load() + g_cancelNotFired.load()), (unsigned long long)g_cancelFired.load(),
             (unsigned long long)g_ranNative.load(), (unsigned long long)g_blocked.load(), (unsigned long long)g_mismatch.load(),
             (unsigned long long)g_superseded.load(), (unsigned long long)g_depthResets.load(),
             (unsigned long long)g_sinkAdds[0].load(), (unsigned long long)g_sinkAdds[1].load(),
             (unsigned long long)g_sinkAdds[2].load(), (unsigned long long)factoryHits);
}
