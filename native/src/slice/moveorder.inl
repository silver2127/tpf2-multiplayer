// moveorder.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// SHIP AND AIRCRAFT CLAIM ORDER (moveorder.h)

// ---------------------------------------------------------------------------
// SHIP AND AIRCRAFT CLAIM ORDER -- measured, not enforced, and here is why.
//
// THE FINDING (RE pass on build 35924). ecs::ShipMoveSystem::Update2 (0xa6c1e0,
// 10,477 bytes) and ecs::AircraftMoveSystem::Update2 (0xa2bc60, 14,604 bytes)
// are both entirely serial and both walk their ECS family's node vector
// linearly -- ship `0xa6c43a mov rax,[r13+8]; mov rsi,[rax]; add rsi,r12` with
// `add r12,0x14` at 0xa6c7e9 and the count in [rbp+0x20]; aircraft the same
// shape at 0xa2beb3 with `add rdx,0x14` at 0xa2c434 and the count in
// [rbp-0x28]. Arbitration is check-then-claim against a persistent map: `call
// IsReserved (0x2114e70)` then, on je, `call Reserve (0x21150b0)` -- ship at
// 0xa6c610/0xa6c650, aircraft at 0xa2c085/0xa2c0c5, with further Reserve sites
// at 0xa2c1f0, 0xa2c3d8 and the runway one at 0xa2f0f4. There is no shuffle
// anywhere in either. The first vehicle in NODE-LIST order to ask for a
// contended lock gets it -- and node-list order is the order the engine
// registered those entities in, which is not replicated state. Two peers
// holding the same world, agreeing on the hash, can send different ships
// through the same lock.
//
// So far this is the train bug. It is not fixed the same way, and that is a
// deliberate decision:
//
// WHAT THE TRAIN PATCH REORDERS, AND WHY THIS CANNOT. TrainMoveSystem::Update2
// builds idx[] = iota over its nodes and shuffles THAT. The train patch sorts a
// scratch array the engine made three instructions earlier, inside the frame it
// was made in, and nothing else in the process can see it. Ship and aircraft
// Update2 have no such array. They index the family node vector directly, so
// the only thing there is to reorder is the ecs::NodeList<4> itself -- the
// caller (ShipMoveSystem::Update, 0xa6ead0; AircraftMoveSystem::Update,
// 0xa2f570) downcasts the INodeList it is handed, points this+8 at its vector,
// derives n from the vector's byte span (/20) and calls Update2 through the
// vtable -- engine-owned, alive across frames, and shared with whatever else
// reads that family. Three things in the disassembly argue against permuting
// it blind:
//   1. Update2 calls ecs::Engine::NoteComponentAboutToBeChanged (0xa6c98d ->
//      0x23e0020) while it is iterating, which is exactly the notification
//      that maintains families. A permutation cannot be reliably undone across a call that
//      may have edited what was permuted.
//   2. The engine re-reads the vector's base pointer from [this+8] on EVERY
//      iteration (0xa6c43a, reloading `this` from [rbp-0x48] at 0xa6c7b1)
//      rather than hoisting it -- the shape of code that expects the vector to
//      move underneath it.
//   3. The scratch arrays Update2 fills are indexed by node POSITION (the
//      per-vehicle speed table at [rbp+0x140], written at 0xa6c5ca as
//      [rax+r15*8]), so a permuted vector would need those permuted with it,
//      and the family's own insert/erase path was not found in this pass.
//      "I did not find an index into it" is not "there is no index into it",
//      and the cost of being wrong is memory corruption on every peer at once.
// None of that can be settled by reading; it wants one run with the permutation
// in and an eye on the family. That run is not available here.
//
// WHAT THIS DOES INSTEAD. It measures, in the form two peers can diff. At each
// Update2 it reads the family's node records (20 bytes: entity id, then four
// component indices), reads each vehicle's Name the way the train patch does,
// and logs an FNV over the NAMES in the engine's current node order. Names are
// replicated (VNAME); entity ids are not, by design. So, on two peers' logs:
//
//     same n, same rankHash=, different nameHash=   ->  the two engines hold
//     the same fleet and are about to claim locks in different orders. That is
//     the desync, caught in the act, with no world hash able to see it.
//
// and `reordered=0` on both says the engine happens to be in name order
// already, which is what the eventual enforcement would make permanent. The
// ordering itself (moveorder.h) is the train rule minus the jitter and is
// computed into a PRIVATE index array; the engine's vector is never written.
//
// THE DETOUR. Both Update2s open `mov rax,rsp; push rbp; push rbx` (5 bytes),
// and `mov rax,rsp` has to run with the caller's rsp, so the relay
// (moveorderrelay_slice.asm) saves every register it could disturb, calls
// MoveOrderObserve, restores them, runs those three instructions itself with
// rsp back at the entry value, and jumps to site+5. No trampoline is called.
//
// KILL SWITCHES: `shiporder=0` and `airorder=0` in tpf2_menu_flags.txt.
// ---------------------------------------------------------------------------
#include "moveorder.h"

static const uintptr_t RVA_SHIP_UPDATE2 = 0xa6c1e0;
static const uintptr_t RVA_AIR_UPDATE2  = 0xa2bc60;
static const int       MOVEORDER_STEAL  = 5;      // `mov rax,rsp` + `push rbp` + `push rbx`

// The two prologues, through the frame anchor. They are the same compiler
// template and differ only in the frame size the `lea rbp` carries.
static const uint8_t MOVEORDER_EXPECT_SHIP[22] = {
    0x48, 0x8B, 0xC4,                    // mov rax, rsp          <- the 5 stolen
    0x55, 0x53,                          // push rbp / push rbx
    0x56, 0x57,                          // push rsi / push rdi
    0x41, 0x54, 0x41, 0x55,              // push r12 / push r13
    0x41, 0x56, 0x41, 0x57,              // push r14 / push r15
    0x48, 0x8D, 0xA8, 0xC8, 0xFA, 0xFF, 0xFF   // lea rbp, [rax-0x538]
};
static const uint8_t MOVEORDER_EXPECT_AIR[22] = {
    0x48, 0x8B, 0xC4,
    0x55, 0x53,
    0x56, 0x57,
    0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xA8, 0x78, 0xFA, 0xFF, 0xFF   // lea rbp, [rax-0x588]
};

extern "C" {
    uint64_t g_shipOrderResume = 0;      // site+5, where the relays jump when done
    uint64_t g_airOrderResume = 0;
    void ShipOrderRelay();
    void AirOrderRelay();
}

struct MoveOrderChan {
    const char* tag;
    std::vector<TrainOrderKey> keys;
    std::vector<int32_t> idx;
    volatile LONG busy, calls, refusals, reordered, maxUs, suppressed;
    volatile LONG64 lastN;
    uint32_t lastNameHash, lastIdHash;
    ULONGLONG lastLogMs;
    bool haveLast, on;
};
static MoveOrderChan g_shipChan;
static MoveOrderChan g_airChan;
static const ULONGLONG MOVEORDER_LOG_GAP_MS = 5000;

static void MoveOrderRefuse(MoveOrderChan& ch, const char* why, int64_t n)
{
    static const char* lastRefused[2] = { nullptr, nullptr };
    const int slot = (&ch == &g_shipChan) ? 0 : 1;
    InterlockedIncrement(&ch.refusals);
    if (why == lastRefused[slot]) return;
    lastRefused[slot] = why;
    Log("[%s] refused: %s (n=%lld) -- nothing measured this step\n",
        ch.tag, why, (long long)n);
}

// Read the family, rank it by name into a private index array, and log when
// anything about the answer changed. Never writes to anything the engine owns.
static void MoveOrderMeasure(MoveOrderChan& ch, void* self, void* world, int64_t n)
{
    InterlockedIncrement(&ch.calls);
    InterlockedExchange64(&ch.lastN, (LONG64)n);
    if (n < 0 || n > MOVEORDER_MAX_N) { MoveOrderRefuse(ch, "bad count", n); return; }
    if (n < 2) return;
    if (!self || !Readable((const uint8_t*)self + 8, 8)) {
        MoveOrderRefuse(ch, "no node list", n); return;
    }
    // this+8 points at the NodeList's vector (begin, end): ShipMoveSystem::Update
    // sets it at 0xa6eb35 (`mov [rsi+8],rdi` with rdi = nodelist+8) and derives
    // n from end-begin at 0xa6ec02, so both must agree here.
    uint8_t* holder = *(uint8_t**)((const uint8_t*)self + 8);
    if (!holder || !Readable(holder, 16)) { MoveOrderRefuse(ch, "no node list", n); return; }
    const uint8_t* recs = *(const uint8_t**)holder;
    const uint8_t* recsEnd = *(const uint8_t**)(holder + 8);
    if (!recs || recsEnd < recs || (int64_t)(recsEnd - recs) != n * MOVEORDER_REC) {
        MoveOrderRefuse(ch, "count does not match the node vector", n); return;
    }
    if (!Readable(recs, (size_t)n * MOVEORDER_REC)) {
        MoveOrderRefuse(ch, "records unreadable", n); return;
    }

    if ((int64_t)ch.keys.size() < n) ch.keys.resize((size_t)n);
    if ((int64_t)ch.idx.size() < n) ch.idx.resize((size_t)n);
    TrainOrderKey* keys = ch.keys.data();
    int32_t* idx = ch.idx.data();
    for (int64_t i = 0; i < n; i++) {
        keys[i].name = nullptr; keys[i].len = 0; keys[i].score = 0;
        keys[i].id = MoveOrderRecId(recs, (int32_t)i);
        idx[i] = (int32_t)i;
    }

    uint8_t* w = (uint8_t*)world;
    const bool worldOk = w && Readable(w + 0x48, 8) && Readable(w + 0x88, 8) &&
                         Readable(w + 0xa0, 8) && *(uint8_t**)(w + 0x88) && *(uint8_t**)(w + 0xa0);
    int noName = 0;
    const int typeIdx = worldOk ? TrainOrderNameType(w) : -1;
    if (typeIdx >= 0) {
        for (int64_t i = 0; i < n; i++) {
            const int slot = TrainOrderSlot(w, keys[i].id, typeIdx);
            const uint8_t* comp = slot < 0 ? nullptr : TrainOrderComponent(w, typeIdx, slot);
            if (!comp || !TrainOrderNameText(comp, &keys[i].name, &keys[i].len)) {
                keys[i].name = nullptr; keys[i].len = 0; noName++;
            }
        }
    } else {
        noName = (int)n;
    }

    // Hashed BEFORE the rank, over the iota, so nameHash and idHash describe
    // the engine's own claim order; rankHash is the same names in name order,
    // i.e. the fleet as a set, equal on two peers that hold the same ships.
    const uint32_t nameHash = MoveOrderNameHash(idx, n, keys);
    const uint32_t idHash = MoveOrderIdHash(idx, n, keys);
    const MoveOrderOutcome o = MoveOrderRank(idx, n, keys);
    if (o.refused) { MoveOrderRefuse(ch, o.refused, n); return; }
    const uint32_t rankHash = MoveOrderNameHash(idx, n, keys);
    if (o.changed) InterlockedIncrement(&ch.reordered);

    // First time, and whenever n or either hash moved -- but a fleet whose
    // vector changes shape every step must not turn this into a per-step log,
    // so a change inside the 5 s window is only counted, and the next line
    // past the window says how many steps went unlogged.
    if (ch.haveLast && ch.lastNameHash == nameHash && ch.lastIdHash == idHash &&
        ch.lastN == (LONG64)n) return;
    const ULONGLONG now = GetTickCount64();
    if (ch.haveLast && now - ch.lastLogMs < MOVEORDER_LOG_GAP_MS) {
        InterlockedIncrement(&ch.suppressed);
        return;
    }
    ch.lastNameHash = nameHash; ch.lastIdHash = idHash; ch.haveLast = true;
    ch.lastLogMs = now;
    const LONG skipped = InterlockedExchange(&ch.suppressed, 0);
    Log("[%s] n=%lld named=%lld unnamed=%d nameHash=%08x rankHash=%08x idHash=%08x "
        "reordered=%d suppressed=%ld%s%s\n",
        ch.tag, (long long)n, (long long)o.named, noName, nameHash, rankHash, idHash,
        o.changed ? 1 : 0, skipped,
        o.duplicates ? " DUPLICATE-IDS" : "",
        typeIdx >= 0 ? "" : " NO-NAMES(world unreadable)");
}

// Called by the relays at the first instruction of Update2, before the engine
// has done anything: busy flag, fault guard and the clock, exactly as the train
// patch does it. A fault costs the measurement for one step and nothing else --
// nothing here is written that the engine reads.
extern "C" void MoveOrderObserve(int kind, void* self, void* world, int n)
{
    MoveOrderChan& ch = kind == 0 ? g_shipChan : g_airChan;
    if (InterlockedCompareExchange(&ch.busy, 1, 0) != 0) {
        InterlockedIncrement(&ch.refusals);
        return;
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    __try {
        MoveOrderMeasure(ch, self, world, (int64_t)n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool told[2] = { false, false };
        const int slot = kind == 0 ? 0 : 1;
        if (!told[slot]) {
            told[slot] = true;
            Log("[%s] faulted -- measurement skipped this step, the game is untouched\n", ch.tag);
        }
    }
    InterlockedExchange(&ch.busy, 0);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0) return;
    const LONG us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    if (us > ch.maxUs) {
        InterlockedExchange(&ch.maxUs, us);
        static bool warned[2] = { false, false };
        const int slot = kind == 0 ? 0 : 1;
        if (us > 1000 && !warned[slot]) {
            warned[slot] = true;
            Log("[%s] SLOW: %ld us for %lld vehicles in one step -- over the 1 ms budget\n",
                ch.tag, us, (long long)ch.lastN);
        }
    }
}

static void InstallMoveOrder(MoveOrderChan& ch, const char* tag, uintptr_t rva,
                             const uint8_t* expect, size_t expectLen,
                             void* relay, uint64_t* resume, const char* what)
{
    ch.tag = tag;
    ch.lastN = -1;
    if (FlagsSayOff(tag)) {
        Log("[%s] OFF (%s=0 in tpf2_menu_flags.txt) -- %s claim order is not measured\n",
            tag, tag, what);
        return;
    }
    if (!BytesAre(rva, expect, expectLen, tag)) return;
    *resume = g_base + rva + MOVEORDER_STEAL;
    if (!PatchJumpNear(g_base + rva, relay, MOVEORDER_STEAL, nullptr)) {
        *resume = 0;
        Log("[%s] NOT installed: could not write the detour at rva=%llx\n",
            tag, (unsigned long long)rva);
        return;
    }
    ch.on = true;
    Log("[%s] installed rva=%llx steal=%d resume=%llx -- %s claim order is measured, NOT "
        "changed: the family node vector is engine-owned (see the header)\n",
        tag, (unsigned long long)rva, MOVEORDER_STEAL,
        (unsigned long long)(rva + MOVEORDER_STEAL), what);
}
