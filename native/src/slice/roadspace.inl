// roadspace.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// near-page detours, ROAD FREE SPACE and road entry order

// ---------------------------------------------------------------------------
// A FIVE-BYTE DETOUR, AND SOMEWHERE NEAR THE EXE TO PUT IT
//
// PatchJump above writes `jmp [rip+0]` and therefore needs 14 bytes. The four
// functions below have one instruction each to spare before something that
// cannot be relocated (a call rel32) or that must run with the original rsp
// (`mov rax,rsp`), so they get the other kind of detour: a 5-byte `jmp rel32`
// into a stub allocated close enough to the exe for a rel32 to reach it, and
// the stub does the far jump. The stub page also holds the trampolines -- the
// stolen 5 bytes plus an absolute jump back to target+5 -- so a detour can hand
// the call back to the engine unchanged when it decides not to act.
//
// Why "near" is not a gamble: the exe is based at 0x140000000 and the window a
// rel32 reaches is +-2 GB around it. The search below walks the free regions
// VirtualQuery reports, up first and then down, and reserves one 4 KB page. If
// it somehow cannot, nothing is patched and the log says so -- the same failure
// mode as a byte guard that does not match.
// ---------------------------------------------------------------------------
static uint8_t* g_nearPage = nullptr;
static size_t   g_nearUsed = 0;
static const size_t NEAR_PAGE_SIZE = 4096;

static bool NearPageInit()
{
    if (g_nearPage) return true;
    if (!g_base) return false;
    const uintptr_t gran = 0x10000;
    const uintptr_t start = g_base & ~(gran - 1);
    const uintptr_t hi = g_base + 0x60000000ull;          // 1.5 GB of headroom
    const uintptr_t lo = g_base > 0x60000000ull ? g_base - 0x60000000ull : gran;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = start; a < hi; ) {
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc((void*)a, NEAR_PAGE_SIZE,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) { g_nearPage = (uint8_t*)p; return true; }
        }
        const uintptr_t next = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize + gran - 1) & ~(gran - 1);
        if (next <= a) break;
        a = next;
    }
    for (uintptr_t a = start; a > lo; a -= gran) {
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        if (mbi.State != MEM_FREE) continue;
        void* p = VirtualAlloc((void*)a, NEAR_PAGE_SIZE,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) { g_nearPage = (uint8_t*)p; return true; }
    }
    return false;
}

static uint8_t* NearAlloc(size_t n)
{
    if (!NearPageInit()) return nullptr;
    n = (n + 15) & ~(size_t)15;
    if (g_nearUsed + n > NEAR_PAGE_SIZE) return nullptr;
    uint8_t* p = g_nearPage + g_nearUsed;
    g_nearUsed += n;
    return p;
}

// Steal exactly `steal` bytes (whole instructions, no rip-relative operand, no
// call/jmp rel32 -- the caller has checked that against the measured bytes)
// and point them at `detour`. With trampolineOut, the stolen bytes are copied
// where the engine can still run them, followed by a jump to target+steal.
static bool PatchJumpNear(uintptr_t at, void* detour, int steal, void** trampolineOut)
{
    if (steal < 5 || steal > 16) return false;
    uint8_t* stub = NearAlloc(14);
    if (!stub) return false;
    uint8_t* tramp = nullptr;
    if (trampolineOut) {
        tramp = NearAlloc((size_t)steal + 14);
        if (!tramp) return false;
    }
    const int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;

    stub[0] = 0xFF; stub[1] = 0x25; memset(stub + 2, 0, 4);       // jmp [rip+0]
    const uintptr_t d = (uintptr_t)detour;
    memcpy(stub + 6, &d, 8);
    if (tramp) {
        memcpy(tramp, (const void*)at, (size_t)steal);
        tramp[steal] = 0xFF; tramp[steal + 1] = 0x25;
        memset(tramp + steal + 2, 0, 4);
        const uintptr_t back = at + (uintptr_t)steal;
        memcpy(tramp + steal + 6, &back, 8);
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)at, (SIZE_T)steal, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t patch[16];
    patch[0] = 0xE9;
    const int32_t r32 = (int32_t)rel;
    memcpy(patch + 1, &r32, 4);
    memset(patch + 5, 0xCC, (size_t)steal - 5);
    memcpy((void*)at, patch, (size_t)steal);
    VirtualProtect((void*)at, (SIZE_T)steal, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, (SIZE_T)steal);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
    if (tramp) FlushInstructionCache(GetCurrentProcess(), tramp, (SIZE_T)steal + 14);
    if (trampolineOut) *trampolineOut = tramp;
    return true;
}

// `<key>=0` in tpf2_menu_flags.txt, the same file (and the same dumb prefix
// match) the menu dll and FlagsSayNoTrainOrder read. Next to this dll first,
// then the data dir.
static bool FlagsSayOff(const char* key)
{
    const size_t klen = strlen(key);
    for (int i = 0; i < 2; i++) {
        const char* dir = i == 0 ? g_dllDir : g_dataDir;
        if (!dir[0]) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", dir);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (!f) continue;
        char line[256]; bool off = false;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, key, klen) && line[klen] == '=' && line[klen + 1] == '0') off = true;
        fclose(f);
        return off;
    }
    return false;
}

// Byte guard shared by the installers below: the bytes at `rva` must be the
// ones measured on this build, or nothing is written and the log says what was
// found instead.
static bool BytesAre(uintptr_t rva, const uint8_t* want, size_t n, const char* tag)
{
    const uintptr_t at = g_base + rva;
    if (Readable((const void*)at, n) && memcmp((const void*)at, want, n) == 0) return true;
    char got[3 * 24 + 1]; got[0] = 0;
    const size_t show = n < 24 ? n : 24;
    if (Readable((const void*)at, show))
        for (size_t i = 0; i < show; i++) snprintf(got + i * 3, 4, "%02x ", ((const uint8_t*)at)[i]);
    Log("[%s] NOT installed: bytes at rva=%llx are not the sequence measured on build %lu "
        "(got: %s)\n", tag, (unsigned long long)rva, (unsigned long)GAME_BUILD_NUMBER,
        got[0] ? got : "unreadable");
    return false;
}

// ---------------------------------------------------------------------------
// ROAD FREE SPACE -- an order-dependent float sum, three instructions from a
// junction decision.
//
// THE FINDING (RE pass on build 35924). Roads do not use the reservation
// manager that trains, ships and aircraft arbitrate with. Their one real order
// dependence is arithmetic. transport::EdgeUseManager::GetUsedSpace (0x2117350,
// and the sibling that takes a skip predicate, 0x2117140) walks one edge's
// `entries` vector, clips each vehicle's footprint to the edge, and adds the
// clipped lengths up in SINGLE precision in vector order:
//
//     0x1421173b0  loop: GetPos, clip, GetPos, clip ...
//     0x142117448  subss xmm1,xmm0        ; hi - lo
//     0x142117450  addss xmm1,xmm7        ; + the running sum
//     0x14211745a  jne 0x1421173b0
//
// `entries` order comes from EdgeUseManager::Add (0x2115f80, a push_back) and
// Remove (0x2117c50, an order-preserving erase), driven by ECS callbacks --
// that is, by the order the engine registered those vehicles in, which is not
// replicated state and which two peers can legitimately disagree about (the
// mod's world hash is geometric: same positions, same edges, equal hash). Float
// addition is not associative. Same vehicles, same footprints, different order,
// one ULP apart. And the consumer, inside the junction decision
// MotionCalculator::GetNextSpeedLimitAndVehicles (0x2213db0), is a bare
// compare:
//
//     0x142214bcd  call GetUsedSpace ; subss xmm6,xmm0       (space left)
//     0x142214e76  comiss xmm12,xmm6 ; jbe -> BrakePoint      ("not enough space")
//
// so that ULP decides whether a bus enters the junction or stops at it. One
// peer goes, the other waits, and nothing in the command stream or the hash
// ever mentions it.
//
// THE PATCH. Both functions are replaced at their first instruction with the
// same algorithm -- same clipping, same GetPos calls per entry in the same
// order, same per-term rounding -- that collects the terms, SORTS them, and
// sums them in double (roadspace.h, which explains why sorting and not just
// widening). The engine's own body stays reachable behind a 5-byte trampoline
// and is what runs whenever the detour meets a shape it does not recognise --
// a bad entry span, more than ROADSPACE_MAX_TERMS vehicles on one edge, a
// predicate object with no target, or a fault while walking -- so a surprise
// costs the determinism for that call, never the answer. The fallback is
// logged once.
//
// THE ABIs, measured off the two prologues and their callers (0x142214bcd,
// 0x140ab5c33, 0x142116845):
//
//   float GetUsedSpace(const EdgeId& id) const
//       rcx = this, rdx = &id. GetEdgeDataPtr (0x2116330; rcx = this,
//       rdx = &id) returns the EdgeData* or null, and null is 0.0f.
//       EdgeData: +0 float length, +8/+0x10/+0x18 the entries vector.
//       GetPos (0x21163c0; rcx = this, edx = entry.comp, xmm2 = length)
//       returns the vehicle's position along the edge in xmm0. Pure: it reads
//       the MovePath component and never writes.
//   float GetUsedSpace(const EdgeId& id,
//                      const std::function<bool(const Entry&, float&)>& skip) const
//       rcx = this, rdx = &id, r8 = &skip. The bool at id+8 picks the
//       direction: only entries whose own `forward` byte matches are counted
//       (loop A at 0x1421171a1 for a set flag, loop B at 0x142117280 for a
//       clear one). `skip` is an MSVC std::function: _Getimpl() is the last
//       pointer of the 64-byte object, at +0x38 (0x1421171cc `mov rcx,
//       [r14+0x38]`), a null one goes to _Xbad_function_call (0x142117344 ->
//       0x2bf61ca), and the call is virtual slot 2, _Do_call, at vtable+0x10
//       (0x1421171ea `call [rax+0x10]`), taking rdx = the entry and r8 = the
//       address of a float the engine then discards. A null target is handed
//       straight back to the engine, so that throw still happens exactly when
//       it did.
//
// COST. No VirtualQuery on this path: GetUsedSpace runs once per road vehicle
// per look-ahead edge per step, so every check here is arithmetic on the
// pointers the engine itself is about to dereference, and a fault is caught by
// the SEH frame and handed to the engine's own body (which then does exactly
// what it would have done today).
//
// KILL SWITCH: `roadspace=0` in tpf2_menu_flags.txt leaves both functions alone.
// ---------------------------------------------------------------------------
#include "roadspace.h"

static const uintptr_t RVA_ROADSPACE_A   = 0x2117350;   // GetUsedSpace(EdgeId)
static const uintptr_t RVA_ROADSPACE_B   = 0x2117140;   // ...with a skip predicate
static const uintptr_t RVA_EDGEUSE_DATA  = 0x2116330;   // GetEdgeDataPtr(this, EdgeId*)
static const uintptr_t RVA_EDGEUSE_POS   = 0x21163c0;   // GetPos(this, compIdx, length)
static const int       ROADSPACE_STEAL   = 5;           // one whole `mov [rsp+d8],reg`

// The prologues, checked well past the five bytes actually stolen: a build that
// matches 18 bytes here is the build these RVAs were measured on.
static const uint8_t ROADSPACE_EXPECT_A[18] = {
    0x48, 0x89, 0x6C, 0x24, 0x20,        // mov [rsp+0x20], rbp   <- the 5 stolen
    0x56,                                // push rsi
    0x48, 0x83, 0xEC, 0x40,              // sub rsp, 0x40
    0x48, 0x8B, 0xE9,                    // mov rbp, rcx
    0xE8, 0xCE, 0xEF, 0xFF, 0xFF         // call 0x2116330
};
static const uint8_t ROADSPACE_EXPECT_B[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,        // mov [rsp+0x18], rbx   <- the 5 stolen
    0x56, 0x57, 0x41, 0x56,              // push rsi / rdi / r14
    0x48, 0x83, 0xEC, 0x40,              // sub rsp, 0x40
    0x4D, 0x8B, 0xF0                     // mov r14, r8
};
// ...and the two helpers the replacement calls, so a byte match that landed on
// some other function cannot pass.
static const uint8_t EDGEUSE_DATA_EXPECT[7] = {
    0x48, 0x83, 0xEC, 0x28,              // sub rsp, 0x28
    0x4C, 0x63, 0x02                     // movsxd r8, dword ptr [rdx]
};
static const uint8_t EDGEUSE_POS_EXPECT[8] = {
    0x48, 0x83, 0xEC, 0x28,              // sub rsp, 0x28
    0x85, 0xD2,                          // test edx, edx
    0x78, 0x6C                           // js  (negative component index)
};

typedef const uint8_t* (*EdgeDataFn)(void*, const void*);
typedef float (*EdgePosFn)(void*, int, float);
typedef float (*RoadSpaceAFn)(void*, const void*);
typedef float (*RoadSpaceBFn)(void*, const void*, void*);

static bool         g_rsOn = false;
static RoadSpaceAFn g_rsOrigA = nullptr;
static RoadSpaceBFn g_rsOrigB = nullptr;
static volatile LONG g_rsCallsA = 0, g_rsCallsB = 0, g_rsDiffs = 0;
static volatile LONG g_rsMaxN = 0, g_rsHanded = 0, g_rsFaults = 0;

static int RoadSpaceSehFilter(unsigned code)
{
    // Only a bad read. A C++ exception from the engine's own skip predicate
    // (0xE06D7363) must keep unwinding through this frame, not be swallowed
    // here and turned into a number.
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                              : EXCEPTION_CONTINUE_SEARCH;
}

// Walk one edge's entries and collect the per-entry terms. Returns false when
// the shape is not the one measured, in which case nothing with a side effect
// has been called (GetEdgeDataPtr and GetPos are pure reads) and the caller
// hands the whole thing back to the engine. `*empty` means the engine's own
// answer would have been 0.0f (no edge data, or no entries).
static bool RoadSpaceCollect(void* self, const void* edgeId, void* fnObj,
                             RoadSpaceAcc* acc, bool* empty)
{
    const EdgeDataFn getData = (EdgeDataFn)(g_base + RVA_EDGEUSE_DATA);
    const EdgePosFn  getPos  = (EdgePosFn)(g_base + RVA_EDGEUSE_POS);
    RoadSpaceBegin(acc);
    *empty = true;
    if (!self || !edgeId) return false;

    // The filtered overload reads the direction out of the EdgeId before it
    // touches anything else (0x142117174 `cmp byte ptr [rbx+8],0`), and the
    // predicate's target pointer before it calls anything.
    bool wantForward = false;
    uint8_t* impl = nullptr;
    if (fnObj) {
        wantForward = *((const uint8_t*)edgeId + 8) != 0;
        impl = *(uint8_t**)((const uint8_t*)fnObj + 0x38);
        if (!impl) return false;                        // null -> the engine throws
    }

    const uint8_t* ed = getData(self, edgeId);
    if (!ed) return true;                               // engine returns 0.0f

    const RoadUseEntry* b = *(const RoadUseEntry* const*)(ed + 8);
    const RoadUseEntry* e = *(const RoadUseEntry* const*)(ed + 0x10);
    if (b == e) return true;                            // no entries: 0.0f
    if (!b || !e || e < b) return false;
    const size_t span = (size_t)((const uint8_t*)e - (const uint8_t*)b);
    if (span % sizeof(RoadUseEntry)) return false;
    if (span / sizeof(RoadUseEntry) > (size_t)ROADSPACE_MAX_TERMS) return false;
    *empty = false;

    typedef bool (*SkipFn)(void*, const void*, float*);
    for (const RoadUseEntry* p = b; p != e; p++) {
        const bool fwd = p->forward != 0;
        if (fnObj) {
            if (fwd != wantForward) continue;
            // The engine reads the edge length fresh for every GetPos call
            // (`movss xmm2,[rsi]`), so this does too: the predicate is engine
            // code and may have moved it.
            float len0; memcpy(&len0, ed, 4);
            const float pos0 = getPos(self, p->comp, len0);
            float v = fwd ? (pos0 + p->boundsFront) : (len0 - (pos0 - p->boundsFront));
            const SkipFn skip = *(SkipFn*)(*(uint8_t**)impl + 0x10);
            if (skip(impl, p, &v)) continue;
        }
        float len1; memcpy(&len1, ed, 4);
        const float pos1 = getPos(self, p->comp, len1);
        float len2; memcpy(&len2, ed, 4);
        const float pos2 = getPos(self, p->comp, len2);
        if (fwd) {
            float len3; memcpy(&len3, ed, 4);
            RoadSpaceAdd(acc, RoadSpaceTermForward(pos1, pos2, p->boundsBack,
                                                   p->boundsFront, len3));
        } else {
            RoadSpaceAdd(acc, RoadSpaceTermBackward(pos1, pos2, p->boundsBack,
                                                    p->boundsFront));
        }
    }
    return true;
}

// Counters for the alive line, and the two lines that are the live proof of
// the finding: the busiest edge seen, and the first time the ordered sum is a
// different float from the one the engine would have returned.
static void RoadSpaceNote(const RoadSpaceAcc* acc, float out, bool filtered)
{
    InterlockedIncrement(filtered ? &g_rsCallsB : &g_rsCallsA);
    if ((LONG)acc->total > g_rsMaxN) InterlockedExchange(&g_rsMaxN, (LONG)acc->total);
    if (acc->n >= 2 && memcmp(&out, &acc->asEngine, sizeof(float)) != 0) {
        InterlockedIncrement(&g_rsDiffs);
        static bool told = false;
        if (!told) {
            told = true;
            Log("[roadspace] first answer changed: %d vehicles on one edge (%s overload), "
                "engine %.9g -> ordered %.9g (delta %.3g) -- this is the ULP that used to "
                "decide a junction\n",
                acc->n, filtered ? "filtered" : "plain", (double)acc->asEngine, (double)out,
                (double)out - (double)acc->asEngine);
        }
    }
}

// Every path that gives the call back to the engine's own body. Logged once.
static void RoadSpaceHanded(const char* why, bool filtered)
{
    InterlockedIncrement(&g_rsHanded);
    static bool said = false;
    if (said) return;
    said = true;
    Log("[roadspace] handed back to the engine (%s overload): %s -- that call's sum "
        "was the engine's own, in vector order. Counted on the alive line from now on\n",
        filtered ? "filtered" : "plain", why);
}

extern "C" float RoadSpaceDetourA(void* self, const void* edgeId)
{
    RoadSpaceAcc acc;
    bool empty = true, ok = false;
    __try {
        ok = RoadSpaceCollect(self, edgeId, nullptr, &acc, &empty);
    } __except (RoadSpaceSehFilter(GetExceptionCode())) {
        InterlockedIncrement(&g_rsFaults);
        RoadSpaceHanded("fault while walking the entries", false);
        return g_rsOrigA(self, edgeId);
    }
    if (!ok) {
        RoadSpaceHanded("entry span, count or edge id not the measured shape", false);
        return g_rsOrigA(self, edgeId);
    }
    if (empty) return 0.0f;
    const float out = RoadSpaceResult(&acc);
    RoadSpaceNote(&acc, out, false);
    return out;
}

extern "C" float RoadSpaceDetourB(void* self, const void* edgeId, void* fnObj)
{
    RoadSpaceAcc acc;
    bool empty = true, ok = false;
    __try {
        ok = RoadSpaceCollect(self, edgeId, fnObj, &acc, &empty);
    } __except (RoadSpaceSehFilter(GetExceptionCode())) {
        InterlockedIncrement(&g_rsFaults);
        RoadSpaceHanded("fault while walking the entries", true);
        return g_rsOrigB(self, edgeId, fnObj);
    }
    if (!ok) {
        RoadSpaceHanded("entry span, count, edge id or predicate not the measured shape", true);
        return g_rsOrigB(self, edgeId, fnObj);
    }
    if (empty) return 0.0f;
    const float out = RoadSpaceResult(&acc);
    RoadSpaceNote(&acc, out, true);
    return out;
}

// ---------------------------------------------------------------------------
// ROAD ENTRY ORDER -- the per-edge `entries` vector in a canonical order.
//
// THE FINDING (RE, 2026-09-16, after a hot join drifted buses ~300 m at equal
// sim time while the world hash stayed locked). EdgeUseManager's per-edge
// `entries` are not serialized: they are rebuilt on load (EntityAdded 0xa64be0
// -> AddToEdgeUseManager 0xa64390 -> Add 0x2115f80, a push_back) in the order
// the loaded save registers vehicles, while the host's vector holds the order
// they ARRIVED on the edge over the whole game. Same vehicles, different order.
// GetUsedSpace's float sum over that order is already neutralised (ROAD FREE
// SPACE above). The other order-sensitive consumer is the lead-vehicle search
// in GetNext (0x2116990/0x2116e10/0x2116160/0x21164d0): an exact-float min
// search that keeps the FIRST entry on a tie, so two vehicles at bit-identical
// positions (queued at a stop) pick a different leader on the two peers, the
// brake point differs, and car-following amplifies it down the line.
//
// THE PATCH. A post-call hook right after AddToEdgeUseManager's call to Add
// (0xa64473, the 8-byte `mov rbx,[rsp+0x80]` that follows it) re-sorts that
// edge's entries by vehicle NAME (the train-order rule: ASCII-case-insensitive
// byte-wise, then entity id), which is replicated state on every peer. Add is
// the only writer that appends (Remove is an order-preserving erase), so the
// vector is canonical after every mutation. At the hook, rsi is still the
// EdgeUseManager, rbx the ecs world (AddToEdgeUseManager uses it for the type
// index map at +0x48) and the EdgeId Add was given is still at [rsp+0x30]. The
// EdgeData is found the engine's way (GetEdgeDataPtr 0x2116330), and a vector
// we cannot make sense of (bad span, more than ROADENTRIES_MAX vehicles on one
// edge, a fault) is left exactly as the engine built it and counted.
//
// Kill switch: roadentries=0 in tpf2_menu_flags.txt.
static const uintptr_t RVA_ROADENTRIES_HOOK = 0xa64473;   // right after `call 0x2115f80` in AddToEdgeUseManager
static const uint8_t ROADENTRIES_EXPECT[8] = { 0x48, 0x8B, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00 };   // mov rbx,[rsp+0x80]
static const uint32_t ROADENTRIES_EDGEID_OFF = 0x30;      // [rsp+0x30] at the hook = the EdgeId passed to Add
static const int      ROADENTRIES_MAX = 512;
static bool g_reOn = false;
static volatile LONG g_reCalls = 0, g_reSorted = 0, g_reRefused = 0, g_reFaults = 0, g_reMaxN = 0, g_reShown = 0;

struct RoadEntryKey { TrainOrderKey k; int32_t pos; };

static bool RoadEntriesLess(const RoadEntryKey& a, const RoadEntryKey& b)
{
    const int c = TrainOrderNameCmp(a.k, b.k);
    if (c) return c < 0;
    return a.k.id < b.k.id;
}

static void RoadEntriesSortImpl(uint8_t* world, uint8_t* mgr, const void* edgeId)
{
    typedef uint8_t* (*GetEdgeData)(void*, const void*);
    uint8_t* ed = ((GetEdgeData)(g_base + RVA_EDGEUSE_DATA))(mgr, edgeId);
    if (!ed || !Readable(ed, 0x20)) return;
    uint8_t* begin = *(uint8_t**)(ed + 8);
    uint8_t* end = *(uint8_t**)(ed + 0x10);
    if (!begin || end < begin || (size_t)(end - begin) % sizeof(RoadUseEntry)) { InterlockedIncrement(&g_reRefused); return; }
    const int64_t n = (int64_t)((end - begin) / sizeof(RoadUseEntry));
    if (n > g_reMaxN) g_reMaxN = (LONG)n;
    if (n < 2) return;
    if (n > ROADENTRIES_MAX || !Readable(begin, (size_t)(end - begin))) { InterlockedIncrement(&g_reRefused); return; }
    RoadEntryKey keys[ROADENTRIES_MAX];
    RoadUseEntry rec[ROADENTRIES_MAX];
    memcpy(rec, begin, (size_t)n * sizeof(RoadUseEntry));
    const int typeIdx = TrainOrderNameType(world);
    for (int64_t i = 0; i < n; i++) {
        keys[i].k.name = ""; keys[i].k.len = 0; keys[i].k.id = rec[i].entity; keys[i].pos = (int32_t)i;
        if (typeIdx >= 0) {
            const int slot = TrainOrderSlot(world, rec[i].entity, typeIdx);
            const uint8_t* comp = slot >= 0 ? TrainOrderComponent(world, typeIdx, slot) : nullptr;
            if (comp) TrainOrderNameText(comp, &keys[i].k.name, &keys[i].k.len);
        }
    }
    // insertion sort (n is a handful of vehicles per edge; stable, no allocation)
    for (int64_t i = 1; i < n; i++) {
        RoadEntryKey t = keys[i];
        int64_t j = i - 1;
        while (j >= 0 && RoadEntriesLess(t, keys[j])) { keys[j + 1] = keys[j]; j--; }
        keys[j + 1] = t;
    }
    bool changed = false;
    for (int64_t i = 0; i < n; i++) if (keys[i].pos != i) { changed = true; break; }
    if (!changed) return;
    for (int64_t i = 0; i < n; i++) memcpy(begin + i * sizeof(RoadUseEntry), &rec[keys[i].pos], sizeof(RoadUseEntry));
    InterlockedIncrement(&g_reSorted);
    if (InterlockedIncrement(&g_reShown) <= 4)
        Log("[roadentries] edge with %lld vehicles re-ordered by name (first now entity %d)\n", (long long)n, rec[keys[0].pos].entity);
}

extern "C" void RoadEntriesSort(uint8_t* world, uint8_t* mgr, const void* edgeId)
{
    InterlockedIncrement(&g_reCalls);
    __try { if (world && mgr && edgeId) RoadEntriesSortImpl(world, mgr, edgeId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_reFaults); }
}

static void InstallRoadEntries()
{
    if (FlagsSayOff("roadentries")) {
        Log("[roadentries] OFF (roadentries=0 in tpf2_menu_flags.txt) -- a road edge's vehicle "
            "entries keep the engine's arrival/load order\n");
        return;
    }
    if (!BytesAre(RVA_ROADENTRIES_HOOK, ROADENTRIES_EXPECT, sizeof(ROADENTRIES_EXPECT), "roadentries")) return;
    // the call right before the hook must be EdgeUseManager::Add
    {
        uint8_t pre[5] = { 0 };
        memcpy(pre, (const void*)(g_base + RVA_ROADENTRIES_HOOK - 5), 5);
        int32_t rel = 0; memcpy(&rel, pre + 1, 4);
        if (pre[0] != 0xE8 || (uintptr_t)((int64_t)(RVA_ROADENTRIES_HOOK - 5) + 5 + rel) != 0x2115f80) {
            Log("[roadentries] NOT installed: the call before rva=%llx is not EdgeUseManager::Add\n", (unsigned long long)RVA_ROADENTRIES_HOOK);
            return;
        }
    }
    uint8_t* stub = NearAlloc(96);
    if (!stub) { Log("[roadentries] NOT installed: no page for the stub\n"); return; }
    const uintptr_t helper = (uintptr_t)&RoadEntriesSort;
    size_t k = 0;
    // r8 = &EdgeId (rsp+0x30 at the hook, BEFORE anything is pushed), rcx = world (rbx), rdx = manager (rsi)
    stub[k++] = 0x4C; stub[k++] = 0x8D; stub[k++] = 0x44; stub[k++] = 0x24; stub[k++] = (uint8_t)ROADENTRIES_EDGEID_OFF; // lea r8,[rsp+0x30]
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xCB;                    // mov rcx, rbx
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xD6;                    // mov rdx, rsi
    stub[k++] = 0x55;                                                        // push rbp
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xEC;                    // mov rbp, rsp
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xE4; stub[k++] = 0xF0;  // and rsp, -16
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xEC; stub[k++] = 0x20;  // sub rsp, 0x20
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &helper, 8); k += 8;// mov rax, RoadEntriesSort
    stub[k++] = 0xFF; stub[k++] = 0xD0;                                      // call rax
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xE5;                    // mov rsp, rbp
    stub[k++] = 0x5D;                                                        // pop rbp
    memcpy(stub + k, ROADENTRIES_EXPECT, sizeof(ROADENTRIES_EXPECT)); k += sizeof(ROADENTRIES_EXPECT);   // mov rbx,[rsp+0x80] (stolen)
    const uintptr_t resume = g_base + RVA_ROADENTRIES_HOOK + sizeof(ROADENTRIES_EXPECT);
    stub[k++] = 0xE9;
    const int32_t rel = (int32_t)((int64_t)resume - (int64_t)((uintptr_t)stub + k + 4));
    memcpy(stub + k, &rel, 4); k += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, k);
    const uintptr_t at = g_base + RVA_ROADENTRIES_HOOK;
    const int64_t nrel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) { Log("[roadentries] NOT installed: stub out of reach\n"); return; }
    DWORD old = 0;
    if (!VirtualProtect((void*)at, 8, PAGE_EXECUTE_READWRITE, &old)) { Log("[roadentries] NOT installed: could not unprotect\n"); return; }
    uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    const int32_t r32 = (int32_t)nrel;
    memcpy(patch + 1, &r32, 4);
    memcpy((void*)at, patch, 8);
    VirtualProtect((void*)at, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 8);
    g_reOn = true;
    Log("[roadentries] installed: a road edge's vehicle entries are kept in name order on every peer (hook at rva=%llx)\n",
        (unsigned long long)RVA_ROADENTRIES_HOOK);
}

static void InstallRoadSpace()
{
    if (FlagsSayOff("roadspace")) {
        Log("[roadspace] OFF (roadspace=0 in tpf2_menu_flags.txt) -- road free space keeps "
            "the engine's single-precision sum, which depends on vector order\n");
        return;
    }
    if (!BytesAre(RVA_ROADSPACE_A, ROADSPACE_EXPECT_A, sizeof(ROADSPACE_EXPECT_A), "roadspace")) return;
    if (!BytesAre(RVA_ROADSPACE_B, ROADSPACE_EXPECT_B, sizeof(ROADSPACE_EXPECT_B), "roadspace")) return;
    if (!BytesAre(RVA_EDGEUSE_DATA, EDGEUSE_DATA_EXPECT, sizeof(EDGEUSE_DATA_EXPECT), "roadspace")) return;
    if (!BytesAre(RVA_EDGEUSE_POS, EDGEUSE_POS_EXPECT, sizeof(EDGEUSE_POS_EXPECT), "roadspace")) return;
    // The call inside the first prologue must really be GetEdgeDataPtr: the
    // rel32 is build-specific, so resolve it rather than trust the byte match.
    int32_t rel = 0;
    memcpy(&rel, ROADSPACE_EXPECT_A + 14, 4);
    const uintptr_t target = (uintptr_t)((int64_t)RVA_ROADSPACE_A + 18 + rel);
    if (target != RVA_EDGEUSE_DATA) {
        Log("[roadspace] NOT installed: the call at rva=%llx resolves to %llx, not the edge "
            "data lookup %llx\n", (unsigned long long)(RVA_ROADSPACE_A + 13),
            (unsigned long long)target, (unsigned long long)RVA_EDGEUSE_DATA);
        return;
    }
    void* trampA = nullptr;
    void* trampB = nullptr;
    if (!PatchJumpNear(g_base + RVA_ROADSPACE_A, (void*)&RoadSpaceDetourA, ROADSPACE_STEAL, &trampA)) {
        Log("[roadspace] NOT installed: could not write the detour at rva=%llx (no page within "
            "reach of a rel32?)\n", (unsigned long long)RVA_ROADSPACE_A);
        return;
    }
    g_rsOrigA = (RoadSpaceAFn)trampA;
    g_rsOn = true;
    if (!PatchJumpNear(g_base + RVA_ROADSPACE_B, (void*)&RoadSpaceDetourB, ROADSPACE_STEAL, &trampB)) {
        // The first one is in and working; the sibling is the rarer path.
        Log("[roadspace] PARTIAL: GetUsedSpace is ordered, the filtered sibling at rva=%llx is "
            "not -- its sum still depends on vector order\n", (unsigned long long)RVA_ROADSPACE_B);
        return;
    }
    g_rsOrigB = (RoadSpaceBFn)trampB;
    Log("[roadspace] installed rva=%llx,%llx steal=%d cap=%d -- free space on an edge is "
        "summed in ascending order in double, so every peer gets the same float\n",
        (unsigned long long)RVA_ROADSPACE_A, (unsigned long long)RVA_ROADSPACE_B,
        ROADSPACE_STEAL, ROADSPACE_MAX_TERMS);
}

// ---------------------------------------------------------------------------
// SHARED STATIONS -- one comparison in the line editor is the whole gate.
//
// THE FINDING (RE pass on build 35924, 2026-09-16). In companies mode a player
// clicking ANOTHER company's station in the line editor got nothing: no stop,
// no message, no sound. Walking the click path from the top:
//
//   UI::LineEditor's "add station" tool is a UI::_anon_46B7A670::StationSelector
//   (vftable 0x3010860) paired with a UI::_anon_46B7A670::StationFilter
//   (vftable 0x3010848). Both are built together in the line editor's creation
//   chain, 0x5fc5bc and 0x5fc83a inside 0x5fc560. The selector asks the filter
//   whether an entity under the cursor may be reported at all (UI::IFilter slot
//   1, through the accept helper at 0x439ea0), so a filter that says no makes
//   the entity invisible to the tool: the click lambdas (0x60bea0, which adds
//   the stop, and 0x60c040, the cursor hint) never see it. THAT is why the
//   rejection is silent -- neither of those two has an owner test of its own.
//
//   StationFilter::IsValid is 0x6095d0. It calls
//   AddStationInputComponentChecker::IsValidInput (0x609b00, funcsig-named),
//   which classifies what was clicked into
//   UI::`anonymous namespace'::ValidAddStationSelection and pairs it with the
//   entity to add: Station -> its station group (0), StationGroup (0), a track
//   or road edge the line may put a waypoint on (1), Town (2), depot/other (3),
//   nothing usable (4). IsValidInput itself has NO owner test -- the earlier
//   note in docs/SHARED_INFRA.md was right about that function and wrong about
//   where to look. The owner test is in its CALLER:
//
//     0x609610  lea  rcx, [rbx+8]
//     0x609614  call 0x8b9e60            ; the engine, off the filter
//     0x609619  lea  rdx, [rsp+0x34]     ; the entity IsValidInput paired
//     0x60961e  mov  rcx, rax
//     0x609621  call 0x472900            ; GetComponentPtr<component::PlayerOwned>
//     0x609626  test rax, rax
//     0x609629  je   0x609605            ; no owner    -> accept
//     0x60962b  mov  eax, [rax]          ; PlayerOwned.player
//     0x60962d  test eax, eax
//     0x60962f  js   0x609605            ; owner < 0   -> accept
//     0x609631  cmp  eax, [rbx+0x28]     ; <-- THE GATE
//     0x609634  je   0x609605            ; same owner  -> accept
//     0x609636  xor  eax, eax            ; another company -> REJECT, silently
//
//   and it runs only for classes 0 and 1 -- exactly the station, station group
//   and line-usable edge cases. Classes 2, 3 and 4 skip it. So the comparison
//   this patch answers can only ever be about a stop the line editor was about
//   to accept, never about anything else.
//
//   StationFilter+0x28 is the local human player entity. It is threaded from
//   UI::CGameUI::CreateUI (0x56a121 `call 0x8bb7f0` -- the view manager's game
//   state -- then `mov edi,[rax+0x214]`) through the line list (0x613340) and
//   the LineEditor constructor (0x5fa970, argument 15 at [rbp+0x1f0]) into the
//   filter (0x5fc857 `mov [rcx+0x28], eax`). GameState+0x214 is the same field
//   two other UI owner gates compare against -- 0x8b3020 `cmp *PlayerOwned,
//   [gameState+0x214]` and 0x8a3c20 -- and the one GameState::Replicate copies.
//
// EVERYTHING DOWNSTREAM WAS WALKED AND HAS NO SUCH TEST. The complete set of
// PlayerOwned readers in the binary is 43 inlined type-descriptor sites plus
// the callers of the two accessors (0x472900, 0xc5e20); none of the ones on
// this path compares two entities' owners:
//   make_cmd::UpdateLine 0x9df4e0 and its sim-thread handler 0x9d9fd0 (variant
//   tag 5) assert only `lineEntity != ecs::Entity()` and write the new Line
//   component -- a foreign station id in a replayed updateLine is NOT stripped;
//   ecs::LineSystem::EntityAdded 0xa43400 asserts only that each stop's station
//   group exists; line_util::GetBestLineAssignment 0x215d660,
//   line_util::CalcSectionPaths 0x215a050, CalcLineStopTerminal 0x96f3b0,
//   FindNextFreeTerminal 0xad40b0, ecs::ComputeTerminalConnectivity 0xa42540,
//   station_util::GetCarriers 0x218d720 and GetTerminalPersonEdges 0x218f370,
//   and the person-side LinesExpander (AddStation 0x977410, VisitLines
//   0x977640) contain no PlayerOwned read at all.
// The owner reads that do exist on neighbouring paths are all "charge or
// attribute to the vehicle's own company" (TransportVehicleSystem::
// ChargeRunningCosts 0xad11d0, HandleVehicleArrived 0xad5f70) or UI "is this
// mine" display gates (UI::GetEntitiesForPlayer 0x73d8d0, the entity window
// 0x8b3020 and 0x8a3c20). ONE genuine gate is left deliberately alone:
// vehicle_util::common::FindPathToDepot's search (the compare at 0x216fa52)
// requires the depot's PlayerOwned to equal the vehicle's, so B's vehicles
// still only ever service in B's own depots.
//
// THE PATCH. Five bytes at 0x609631 -- the `cmp` and the `je`, two whole
// instructions, nothing branches into them -- become a jump into a stub that
// asks a helper and then jumps to the engine's own accept (0x609605) or reject
// (0x609636) label. The helper answers "same owner" for a foreign owner ONLY
// while companies mode is live (line 1 of mp_company_cfg.txt, the file the menu
// dll writes at session start and companies.lua reads); otherwise it replays
// the engine's comparison exactly. Outside companies mode there is only one
// player entity, so that comparison cannot fail and the patch is a no-op --
// the mode check is belt and braces, not the safety.
//
// At 0x609631 the function has done `push rbx; sub rsp,0x20`, so rsp is
// 16-aligned and a call from the stub is ABI-correct; rbx is the filter and is
// preserved; eax is dead on both branch targets (they set it themselves), and
// no other register is read after this point, so the stub may clobber rax, rcx
// and rdx freely.
//
// WHAT THIS DOES NOT DO. Ownership does not change: the station stays A's, the
// mod still refuses B's edits and demolition (shared_infra.lua cmMayModify),
// station maintenance stays on A's books and the line's income stays on B's.
//
// KILL SWITCH: `sharedstations=0` in tpf2_menu_flags.txt leaves the comparison
// alone and foreign stations stay unusable.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_SHAREDSTATIONS_GUARD  = 0x609610;  // start of the owner test
static const uintptr_t RVA_SHAREDSTATIONS_SITE   = 0x609631;  // cmp eax,[rbx+0x28]
static const uintptr_t RVA_SHAREDSTATIONS_ACCEPT = 0x609605;  // mov eax,1; ret
static const uintptr_t RVA_SHAREDSTATIONS_REJECT = 0x609636;  // xor eax,eax; ret
static const uintptr_t RVA_SS_GETENGINE          = 0x8b9e60;  // the filter's engine accessor
static const uintptr_t RVA_SS_GETPLAYEROWNED     = 0x472900;  // GetComponentPtr<PlayerOwned>
static const int       SHAREDSTATIONS_STEAL      = 5;         // cmp (3) + je (2)

// The whole test, from the engine lookup to the two labels the stub jumps to.
// A build that matches all 46 bytes is the build these RVAs were measured on.
static const uint8_t SHAREDSTATIONS_EXPECT[46] = {
    0x48, 0x8D, 0x4B, 0x08,              // lea  rcx, [rbx+8]
    0xE8, 0x47, 0x08, 0x2B, 0x00,        // call 0x8b9e60
    0x48, 0x8D, 0x54, 0x24, 0x34,        // lea  rdx, [rsp+0x34]
    0x48, 0x8B, 0xC8,                    // mov  rcx, rax
    0xE8, 0xDA, 0x92, 0xE6, 0xFF,        // call 0x472900
    0x48, 0x85, 0xC0,                    // test rax, rax
    0x74, 0xDA,                          // je   0x609605
    0x8B, 0x00,                          // mov  eax, [rax]
    0x85, 0xC0,                          // test eax, eax
    0x78, 0xD4,                          // js   0x609605
    0x3B, 0x43, 0x28,                    // cmp  eax, [rbx+0x28]   <- the 5 stolen
    0x74, 0xCF,                          // je   0x609605
    0x33, 0xC0,                          // xor  eax, eax
    0x48, 0x83, 0xC4, 0x20,              // add  rsp, 0x20
    0x5B,                                // pop  rbx
    0xC3                                 // ret
};

static bool  g_ssOn = false;
static long  g_ssCalls = 0;     // foreign owners this filter was asked about
static long  g_ssOpened = 0;    // ...of which were let through
static long  g_ssRefused = 0;   // ...refused by the station permissions (mp_company_perms.txt)
static bool  g_ssSaidOnce = false;
