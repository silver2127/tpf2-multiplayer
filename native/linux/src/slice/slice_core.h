// slice_core.h -- what the six slice decoder areas build on in tpf2_slice.so
// (the Linux port of slice_hook.cpp's infrastructure). The facts behind every
// address and layout below are in docs/re/linux/SLICE_CORE.md; the claim ids
// (C-ADD-3, ...) point there.
//
// THE MODEL
//   A player action reaches the game as   factory(Command* slot, ...)   followed,
//   in the same caller and on the same thread, by
//   CommandList::Add(ret, list, Command* cmd, std::function* done, weak_ptr* progress)
//   with cmd == the factory's slot (C-PTR-1, C-PTR-4). A decoder reads the
//   command at the factory, ships it to the inject file, and -- in a live session,
//   at a call site whose Command is known to reach Add (C-PTR-5) -- arms a cancel
//   for that Command pointer. slice-core skips the matching Add (C-ADD-10), fires or
//   does not fire `done` per the arm's policy, and tells the decoder whether the
//   cancel landed so it can write or drop what depends on it.
//
// OWNERSHIP OF PATCHES (C-HOOK-5)
//   slice-core owns ONE detour per target: CommandList::Add and each of the 37
//   factories. Decoders never install those themselves; they register handlers
//   behind them (SliceOnFactory, SliceOnAdd). A target nobody registered a handler
//   for is not patched. Anything else a decoder must hook (e.g. slice-time's
//   UI::Clock::DoStep) goes through SliceRegisterHook, which verifies the bytes
//   and refuses targets owned by slice-core or by another library.
//
// WHERE TO REGISTER
//   From the area's own file, with SLICE_AREA:
//       SLICE_AREA(slice_lines, "slice-lines")
//       {
//           SliceOnFactory({ "slice-lines", kRvaSetLine, OnSetLine, nullptr, nullptr, true });
//       }
//   The body runs once on slice-core's init thread, after the build gate and the
//   reader self-test and before anything is patched. Registration calls made at
//   any other time are refused.
//
// RULES FOR HANDLERS (they run on game threads, inside game calls)
//   - No C++ exception may leave a handler, and a handler must not call game code
//     that can throw: our static libstdc++ and libgcc must never unwind a game
//     exception, and one that unwinds through a handler skips slice-core's
//     bookkeeping (every hook on that thread is bypassed until a later hooked call
//     from a shallower stack frame notices the dead frame; logged, and not always
//     noticed). Read game memory through the guarded readers instead. A game
//     exception thrown by the original function itself passes through the detours
//     untouched: they hold no cleanups.
//   - Keep the pass-through path cheap: Add runs for every command in the game.
//   - Threads are not established (C-ADD-12): keep shared state in atomics or
//     behind a mutex. A cancel arm is per thread by design.
#pragma once
#include <cstddef>
#include <string>
#include <cstdint>
#include <xmmintrin.h>

// ---- image -------------------------------------------------------------------
// Build 35924 only (C-BUILD-1): when the GNU build-id differs slice-core patches
// nothing and never runs a registration.
uintptr_t SliceImageBase();                        // 0 before init
inline uintptr_t SliceAddr(uintptr_t rva) { return SliceImageBase() + rva; }
uintptr_t SliceRvaOf(uintptr_t addr);              // addr - base inside the image, else UINTPTR_MAX

// ---- log: <data dir>tpf2_slice.log, truncated when the game starts -----------------
// One write(2) per call, safe from any thread; a line longer than 4 KB is cut and
// ends in "...". End every message with \n.
void SliceLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int SliceTid();                                    // gettid(), for "which thread" log lines
uint64_t SliceNowMs();                             // CLOCK_MONOTONIC in ms

// ---- data dir and cfg ------------------------------------------------------------
const char* SliceDataDir();                        // with a trailing slash; "" before init
// tpf2_slice.cfg: unchanged Windows Lua order, game folder (cwd) then data dir.
// Only exact key=0 or key=1 at line start (optional trailing blanks) counts.
// Nothing that changes what replicates may be configured here.
bool SliceCfgFlag(const char* key, bool def);
bool SliceDumpPropOn();                            // "dumpprop", re-read at most every 2 s

// ---- identity and session ----------------------------------------------------------
// The instance letter from line 1 of <data dir>tpf2_instance.txt, RE-READ on every
// call (the lobby can switch the letter at run time; slice_hook.cpp ReadInstance).
// Refused (false, out = "") when the file is missing, the letter is not 1-7 ASCII
// letters or digits, or its pid= line is missing or names another process. A
// port= line is optional and ignored (C-FILE-2).
bool SliceInstance(char* out, size_t cap);
// A peer/roster establishes the session; missing or stale heartbeat thereafter
// cannot release the latch. A fresh solo status follows the existing .22 contract.
// There is no alternate multiplayer execution mode.
bool SliceSessionLive();
// Verified direct player Add callers, excluding simulation and save/menu work.
bool SlicePlayerAddSite(uintptr_t addRetRva);

// ---- inject file: <data dir>lockstep_inject_<L>.txt -------------------------------
// One write per record, with the existing release 0.4.22 ARMED line included.
struct SliceRecord {
    char* data;     // malloc'd, NUL-terminated
    size_t len;
    size_t cap;
    bool failed;
};
void SliceRecordAppend(SliceRecord* r, const char* s, size_t n);
void SliceRecordPrintf(SliceRecord* r, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void SliceRecordFree(SliceRecord* r);
// Zero is retained only to reject old callers, never written. None is for
// control/diagnostic messages; One is a cancelled player-action transaction.
enum class SliceArmedLine : uint8_t { None, Zero, One };
enum class SliceInjectResult : uint8_t {
    Written,
    NotWritten,
    Partial // Keep cancellation: Lua may already have consumed a complete prefix.
};
SliceInjectResult SliceInjectWrite(const SliceRecord& r, SliceArmedLine armed);
bool SliceInjectLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- guarded reads (C-READ-1, C-READ-2) ------------------------------------------
// There is no SEH: every read of game memory whose validity is not established
// statically goes through these. They copy through process_vm_readv on our own
// pid, or through a pipe write when that is unavailable, whichever passed the
// self-test at init. When neither passed every reader returns false, and so does
// SliceCancelAvailable (the log says which).
bool SliceReadsOk();
bool SliceRead(uintptr_t addr, void* out, size_t n);           // all n bytes, or false
bool SliceReadable(uintptr_t addr, size_t n);                  // every page of the range is readable
template <class T> inline bool SliceReadT(uintptr_t addr, T* out) { return SliceRead(addr, out, sizeof(T)); }

// libstdc++ containers as the game lays them out (C-STL-1..4; our own libstdc++
// has the same layout, which is what the off-game tests check against).
// std::string {char* p; size_t len; char buf[16]}: copies the text plus its NUL
// into out (cap >= len + 1). Refuses an implausible object: len > maxLen, a short
// string longer than 15, a heap string whose capacity is below len, or text not
// NUL-terminated where libstdc++ always terminates it.
constexpr size_t SliceSanityBytes = size_t(1) << 30; // corrupt span guard, not a content count
bool SliceReadStdString(uintptr_t obj, char* out, size_t cap, size_t* lenOut, size_t maxLen = 1 << 20);
inline bool SliceReadStdString(uintptr_t obj, std::string* out)
{
    uint64_t h[3];
    if (!SliceRead(obj, h, sizeof(h)) || h[1] >= SliceSanityBytes) return false;
    std::string value(size_t(h[1]) + 1, '\0');
    size_t len;
    if (!SliceReadStdString(obj, &value[0], value.size(), &len, SliceSanityBytes)) return false;
    value.resize(len);
    *out = std::move(value);
    return true;
}

// std::vector<T> {begin, end, cap}: checks begin <= end <= cap, both spans a whole
// number of `stride`, count <= maxCount and the element range readable.
struct SliceVec { uintptr_t begin; size_t count; };
bool SliceReadStdVector(uintptr_t obj, size_t stride, size_t maxCount, SliceVec* out);
// std::map / std::set (_Rb_tree): header at obj+8, node_count at obj+0x28; a node
// is {color, parent +8, left +0x10, right +0x18, value +0x20}. Visits values in key
// order. Fails (after visiting what it reached) on an unreadable link, a count
// above maxNodes, a walk longer than the count, or a count that does not match
// the nodes found. visit returns false to stop early (the walk then returns true).
using SliceRbVisit = bool (*)(uintptr_t node, uintptr_t value, void* ctx);
bool SliceWalkStdMap(uintptr_t obj, size_t maxNodes, SliceRbVisit visit, void* ctx, size_t* visited);
// std::function (32 B): _M_manager at +0x10 (0 = empty), _M_invoker at +0x18 (C-STL-3).
bool SliceStdFunctionParts(uintptr_t fn, uintptr_t* manager, uintptr_t* invoker);

// ---- factories and callers -------------------------------------------------------
// The 37 command factories of build 35924 (C-FAC-1, Appendix B), each with the
// variant tag it writes, its prologue and its steal.
struct SliceFactoryInfo {
    uintptr_t rva;
    const char* name;         // make_cmd:: name, or "tag_0xNN" where the map has none
    uint8_t tag;              // CmdData variant index at [[cmd]+0xd48] (C-CMD-2)
    int8_t winId;             // the Windows slice hook id, for comparable logs; -1 none
    uint8_t steal;            // bytes the entry patch replaces (C-FAC-2)
    bool stackArgs;           // reads stack arguments: no generic detour (C-FAC-4)
    uint8_t prologue[18];     // the first `steal` bytes, verified before patching
};
const SliceFactoryInfo* SliceFactories(size_t* count);
const SliceFactoryInfo* SliceFactoryByRva(uintptr_t rva);
const SliceFactoryInfo* SliceFactoryByTag(int tag);
// A factory return address inside the game's script command interface: the 34
// api.cmd.make.* / legacy AddFunction call sites (C-SCRIPT-1, C-SCRIPT-2). A command
// built there is a Lua replay or a mod's own command, never a player's click:
// never capture it, and slice-core refuses to arm it.
bool SliceIsScriptCaller(uintptr_t retRva);
// A call of `factoryRva` returning to `retRva` whose Command reaches CommandList::Add
// on every normal path with no other call in between (C-PTR-5): the 87 UI/engine
// sites whose next call is Add, and CreateLine inside line_util (0x2eb1c49), whose
// two callers call Add next. Verified against the image at init. Every other return
// address is refused by SliceArmCancel: at the two CreateConstructionMenu sites
// (0x1045737, 0x1045a47) the Command is pushed into a vector and destroyed, and the
// copies reach Add later; a call from outside the image is not a player's click.
bool SliceIsArmableSite(uintptr_t retRva, uintptr_t factoryRva);
// For log lines only (C-SCRIPT-4): "script-sink" (0xa2f5c2, 0x11225a9),
// "legacy-script" (0x1d873b2) or "ui". Which Lua state the mod's commands use is not
// established, so nothing may be decided on this.
const char* SliceAddCallerKind(uintptr_t addRetRva);
// [[cmd]+0xd48] through the guarded reader; -1 when unreadable (C-CMD-1).
int SliceCommandTag(uintptr_t cmd);

// ---- factory handlers ----------------------------------------------------------------
// The registers at the factory's entry, as the game passed them. The generic
// detour forwards all six integer and eight SSE argument registers unchanged, which
// is the whole argument list of every factory without stack arguments (C-FAC-4).
// What each register means belongs to the decoder area (C-ABI-1): the Command slot
// is always rdi; a by-value CVec3f is xmm0 = {x, y}, xmm1 = {z}.
struct SliceFactoryCall {
    const SliceFactoryInfo* factory;
    uint64_t rdi, rsi, rdx, rcx, r8, r9;
    __m128 xmm[8];
    uintptr_t retAddr;        // the game caller's return address
    uintptr_t retRva;         // retAddr - base (the routing key the Windows code used)
    bool script;              // SliceIsScriptCaller(retRva)
    bool armable;             // SliceIsArmableSite(retRva, factory->rva)
    uint64_t rax;             // the factory's return value (== rdi), return handlers only
    int tid;
};
inline float SliceXmmFloat(const SliceFactoryCall& c, int reg, int lane)
{
    float v[4];
    _mm_storeu_ps(v, c.xmm[reg]);
    return v[lane];
}

// Read-only: every handler of one call sees the same object, and slice-core takes
// the original's registers and the arm's pointer, factory and caller from its own
// copy, never from this one.
using SliceFactoryFn = void (*)(const SliceFactoryCall& call, void* ctx);
struct SliceFactoryHandler {
    const char* area;
    uintptr_t factoryRva;     // one of SliceFactories()
    SliceFactoryFn onEntry;   // before the original runs: the arguments are intact (may be null)
    SliceFactoryFn onReturn;  // after it returned: the Command in rdi is built (may be null)
    void* ctx;
    bool arms;                // the handler may call SliceArmCancel (logged at install)
    int order;                // lower runs first on the same factory; ties keep registration order.
                              // Areas register in name order, so a cross-area sequence (e.g. the
                              // terrain injectors before MergeTemplateStreet on BuildProposal's Lua
                              // caller, SLICE_CONSTRUCTION.md section 10) must be set here.
};
// Refused (false, logged) outside SLICE_AREA, for an unknown RVA, or for a factory
// that reads stack arguments. Handlers of one factory run by ascending `order`, ties
// in registration order. A handler never runs re-entrantly: a hooked function called
// from inside a handler on the same thread runs without handlers.
bool SliceOnFactory(const SliceFactoryHandler& h);

// ---- the cancel at CommandList::Add ---------------------------------------------------
struct SliceAddCall {
    void* ret;                // Connection* return slot (8 B)
    void* list;               // CommandList*
    void* cmd;                // Command* (0x38 B)
    void* done;               // std::function<void(const Command&)>*, may be empty
    void* progress;           // std::weak_ptr<CmdProgress>*
    uintptr_t retAddr, retRva;
    int tid;
};
// Every Add, before the cancel decision, on the calling thread. Keep it cheap.
using SliceAddObserverFn = void (*)(const SliceAddCall& add, void* ctx);
bool SliceOnAdd(const char* area, SliceAddObserverFn fn, void* ctx);
// During this Add's observer pass only: pass an alternate callback to the
// game's original Add without overwriting/destroying the caller-owned object.
// The complete 32-byte object must be readable, with both manager and invoker
// inside game code. The owner keeps it alive through Add's move. `after` runs
// only after original Add returns; when Add throws the owner retains its storage.
bool SliceOverrideAddDone(const SliceAddCall& add, void* done,
                          void (*after)(void* done, void* ctx) = nullptr, void* ctx = nullptr);

// What to do with `done` when the armed command is skipped (C-ADD-7, C-ADD-11; the
// per-tool choice is the decoder's).
enum class SliceDone : uint8_t {
    Never,       // fire-and-forget: never fire it (SetLine, Reverse, ...); the UI would read
                 // the un-applied command as a failure
    IfPresent,   // fire a non-empty one; an empty one has nobody waiting on it
    Required,    // the tool waits on it: an empty or unfireable one means "do not skip Add"
                 // unless honourArmed
};
enum class SliceOutcome : uint8_t {
    CancelledFired,     // Add skipped, done fired first
    CancelledNotFired,  // Add skipped, done not fired (Never, empty IfPresent, or honourArmed)
    Blocked,            // Strict mode refused capture; original Add did not run. No replay record.
    RanNatively,        // the Command matched but the cancel was refused (tag mismatch, done
                        // not fireable without honourArmed, cancel point gone); Add ran
    Mismatch,           // another Command reached Add first on this thread: arm dropped, Add ran
    Superseded,         // dropped before any Add: re-armed on this thread, or a new top-level
                        // factory call began on it (a stale arm: its factory threw, or its
                        // handler's stack was unwound). That Command did not reach Add
};
const char* SliceOutcomeName(SliceOutcome o);
// add is null for Superseded. Runs on the arming thread.
using SliceLandedFn = void (*)(const SliceAddCall* add, SliceOutcome outcome, void* ctx);
using SliceBeforeFireFn = void (*)(const SliceAddCall& add, void* ctx);
using SlicePrepareCancelFn = bool (*)(const SliceAddCall& add, void* ctx);
struct SliceArm {
    const char* what;             // for the log; a string literal
    SliceDone done;
    bool honourArmed;             // ARMED 1 is already on disk: skip Add even when done could not be
                                  // fired (running it too would apply the action twice)
    SliceBeforeFireFn beforeFire; // right before done fires (e.g. reset a result slot); may be null
    SliceLandedFn landed;         // the outcome; may be null
    void* ctx;
    // Called after tag/callback validation, before any callback is fired or
    // Add is skipped. False blocks the action in strict multiplayer. Capture areas
    // commit their record here so a failed write never loses the local action.
    SlicePrepareCancelFn prepareCancel = nullptr;
};
// Add is hooked, Connection() at 0x3190240 verified and the guarded readers work.
// False means every SliceArmCancel is refused; multiplayer must remain blocked.
bool SliceCancelAvailable();
// Arms the cancel for the factory's Command slot on this thread. Only from inside a
// SliceOnFactory handler of this thread, for its own `call`. Refused (false, nothing
// armed, logged) for a call site that is not armable (SliceIsArmableSite), a script
// caller, a null slot, or while SliceCancelAvailable() is false. The first
// CommandList::Add on this thread consumes the arm: the same Command pointer with the
// factory's tag is skipped per the policy, anything else runs (C-PTR-4). An arm still
// pending when the next top-level factory call starts on the thread is dropped
// (Superseded).
bool SliceArmCancel(const SliceFactoryCall& call, const SliceArm& arm);
// Drops this thread's arm when it is for this call's Command slot, without calling `landed`.
void SliceDisarm(const SliceFactoryCall& call);
// Convenience for capture areas with an immediate, self-contained record.
// Multiplayer always arms and ships One; failed arms/writes leave no record
// and the central Add barrier blocks the click. Solo writes nothing.
// Stateful tools use prepareCancel to commit after callback validation instead.
bool SliceShipAndArm(const SliceFactoryCall& call, const SliceArm& arm, const SliceRecord& rec);

// ---- other hooks -----------------------------------------------------------------------
// A decoder's own typed detour on a target slice-core does not own. Refused when
// the stolen range overlaps one of the factories, Add or Connection(), another
// registered hook, or a site another library of ours patches, or when `steal`
// is not where PrologueSteal lands. Separate sites on one page are safe because
// all shipped patchers write through /proc/self/mem without changing permissions. At install the
// live bytes must equal `expected` (at least `steal` bytes), or the hook is skipped
// with a log line. *trampoline is published before the patch is written
// (hook_posix.cpp).
struct SliceHookSpec {
    const char* area;
    const char* name;
    int winId;                    // -1 none
    uintptr_t rva;
    const uint8_t* expected;
    size_t expectedLen;
    int steal;
    void* detour;
    void** trampoline;
    bool needsCancel;             // install only when SliceCancelAvailable() after Add's install
};
bool SliceRegisterHook(const SliceHookSpec& spec);
// True only after this registered auxiliary hook was successfully installed.
// A non-null trampoline alone is insufficient: it is deliberately retained
// after a possibly visible partial patch, even when installation returned false.
bool SliceHookInstalled(uintptr_t rva);

// ---- registration entry --------------------------------------------------------------------
struct SliceAreaEntry {
    const char* name;
    void (*registerFn)();
};
// Places one entry in the tpf2mp_slice_areas section; slice-core calls every entry
// of the linked library, sorted by name. No constructor and no link-order
// dependency: the section is data the loader has already relocated. At most 64
// entries (slice-core's own included); more and none of them runs (logged).
#define SLICE_AREA(ident, areaName)                                                      \
    static void ident##_SliceRegister();                                                 \
    __attribute__((used, section("tpf2mp_slice_areas")))                                 \
    static const SliceAreaEntry ident##_SliceAreaEntry = { areaName, &ident##_SliceRegister }; \
    static void ident##_SliceRegister()
