// trainorder.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// TRAIN RESERVATION ORDER (trainorder.h)

// ---------------------------------------------------------------------------
// TRAIN RESERVATION ORDER -- a desync the world hash cannot see.
//
// THE FINDING (RE pass on build 35924). ecs::TrainMoveSystem::Update2
// (0xabdc20) decides, once per sim step, which train gets to reserve track
// first. It builds idx[0..n-1] = iota over its train node list (0xabdfb0), then
// at 0xabe02d reads an int out of the GameTime component (Engine::GetComponent
// 0x281250 -> the accessor 0x2877c0 -> ecs::component::GameTime + 0x30), folds
// it into a std::minstd_rand (`s %= 2147483647; if (!s) s = 1` at 0xabe03d,
// then `imul r8, rax, 0xbc8f` mod 0x7fffffff at 0xabe0f0), and Fisher-Yates
// shuffles idx with it (0xabe170). The loop that follows walks the shuffled idx
// and calls transport::EdgeReservationManager::Reserve (0x21150b0) at 0xabe33a
// and 0xabe583 -- serially, on the sim thread, before the parallel movement
// loop at 0xabe7fd. First train through a junction wins it.
//
// The seed is lockstep state: same game time, same seed, same permutation. What
// is NOT lockstep state is what the permutation is applied TO. idx holds
// POSITIONS in the engine's node list, and that list is in the order the engine
// registered the train entities -- not id order, not anything the mod controls,
// and (a multi-threaded save load registers in thread-timing order) not
// necessarily the same on two peers. The mod's world hash is geometric: same
// positions, same edges, same nodes, equal hash. So two peers can hold
// identical worlds, agree on the hash, agree on the seed, and still rank the
// same two trains in opposite orders -- after which one queues two trains on a
// track the other lets through, and the divergence is real and permanent.
// Observed: a desync with no command for 1,450 game units, identical positions
// and ids, equal hash, exactly that symptom.
//
// THE PATCH. Detour 0xabe02d, replace both the seeding and the shuffle with an
// order computed from lockstep state only, and rejoin the engine at 0xabe194 --
// the two instructions that reload rsi/rcx before the reservation loop, past
// its Fisher-Yates. The order (trainorder.h, TrainOrderArrange):
//
//     rank r  = sorted by NAME (ASCII-case-insensitive, byte-wise), then id
//     jitter j = minstd_rand(GameTime+0x30) drawn in rank order, mod n/3
//     reserve in order of (r + j), ties by id
//
// so the alphabet decides who generally goes first while nobody at a busy
// junction can be starved forever -- see trainorder.h for why a strict sort was
// not enough. Every input (the set of trains, their names, the game-time seed)
// is replicated state; the node-list order is not consulted at all.
//
// READING THE NAME. There is no getter to borrow: the engine inlines
// GetComponent<Name> everywhere (58 sites; 0x45ff46 is a clean one, and the
// type_info .?AUName@component@ecs@@ it leas lives at 0x41d4c40). Measured off
// that site, a component read is three steps:
//   1. type index  = 0xd0a40(world + 0x48, &type_info)   -- a map lookup
//   2. slot        = scan world[0xa0][entity] -- a {type index, slot} list
//   3. component   = world[0x88][type index] -> +0x68 + slot * 0x20
//                    (or, for slot >= 0x40000000, the paged table at +0x80)
// and the Name component is a bare std::string: size at +0x10, capacity at
// +0x18, text inline at +0 until capacity reaches 16 -- the same MSVC layout
// this file already decodes for SetName (fid 14).
//
// STEP 2 IS DONE BY HAND ON PURPOSE. The engine's own helper for it (0xd0920)
// ends its scan with a formatted assert and an int3 when the entity has no
// component of that type: calling it for an unnamed train would not return a
// null, it would take the game down. The loop at 0xd0966 it replaces is six
// instructions, and ours stops at the end of the list instead.
//
// `world` is Update2's second argument (r13, assigned at 0xabdc6a). The
// engine's own prologue calls 0xd0a40 on r13+0x48 five times before we get
// here, so by the time the detour runs that registry has already been proved
// good by the code we are standing in.
//
// WHAT THIS ASSUMES, AND WHAT HAPPENS WHEN THE ASSUMPTION IS WRONG. Entity ids
// are NOT equal across peers by design -- replication ships positions, not ids,
// and each peer's allocator is also advanced by town growth it did on its own
// (hash-world-by-geometry-not-ids). Names are the primary key precisely because
// they ARE replicated (VNAME), but they are a weak key: trains share names, and
// every tie falls through to the id. What the tie-break needs is weaker than
// equal ids -- only that the RANK of the ids agrees, which holds while ids are
// handed out in creation order and every train is created by a replicated
// command, but not necessarily after the engine recycles a freed id
// (reused-entity-ids-hide-replayed-constructions).
//
// So the log carries `ids=`, an FNV of the entity ids in final order: equal on
// both peers is proof they are about to let the same trains through in the same
// order. The Lua side hashes the names themselves in a lane of their own
// (hash.lua, the `r:` lane), so a name that differs between peers is reported
// instead of quietly splitting the two simulations here.
//
// KILL SWITCH: `trainorder=0` in tpf2_menu_flags.txt (next to the dlls, or in
// the data dir) skips the patch entirely, leaving the engine's shuffle in
// place. Logged either way.
// ---------------------------------------------------------------------------
#include "trainorder.h"

static const uintptr_t RVA_TRAINORDER_HOOK   = 0xabe02d;   // mov rax,[rsi+0x48]
static const uintptr_t RVA_TRAINORDER_RESUME = 0xabe194;   // past the engine's shuffle
static const uintptr_t RVA_GAMETIME_GET      = 0x2877c0;   // int GameTime::get(void*) -> +0x30
static const uintptr_t RVA_ECS_TYPEINDEX     = 0x0d0a40;   // int(registry, type_info**)
static const uintptr_t RVA_TI_NAME           = 0x41d4c40;  // .?AUName@component@ecs@@
static const int       TRAINORDER_STEAL      = 16;         // 4 + 4 + 5 + 3, a clean boundary

// The 16 bytes the patch overwrites. Checked before anything is written: a
// different build must be left alone, not jmp'd into the middle of.
static const uint8_t TRAINORDER_EXPECT[TRAINORDER_STEAL] = {
    0x48, 0x8B, 0x46, 0x48,              // mov rax, [rsi+0x48]
    0x48, 0x8B, 0x48, 0x18,              // mov rcx, [rax+0x18]
    0xE8, 0x86, 0x97, 0x7C, 0xFF,        // call 0x2877c0
    0x44, 0x8B, 0xC0                     // mov r8d, eax
};
// ...and the instructions the relay resumes on, which must still be the two
// reloads the engine does on the way out of its shuffle.
static const uint8_t TRAINORDER_EXPECT_RESUME[9] = {
    0x48, 0x8B, 0x74, 0x24, 0x78,        // mov rsi, [rsp+0x78]
    0x48, 0x8B, 0x4D, 0xE8               // mov rcx, [rbp-0x18]
};

extern "C" {
    uint64_t g_trainOrderResume = 0;     // where the relay jumps when it is done
    void TrainOrderRelay();
}
static bool g_trainOrderOn = false;
static volatile LONG   g_toCalls = 0, g_toReorders = 0, g_toRefusals = 0;
static volatile LONG   g_toLastSeed = 0, g_toMaxUs = 0;
static volatile LONG64 g_toLastN = -1;

// The int the engine seeds its shuffle with, read the way the stolen bytes read
// it: rcx = *(*(this+0x48)+0x18), then the accessor. Returns 0 if the chain
// does not look like memory; TrainOrderSeedFix turns that into 1, so the order
// stays defined (and both peers get the same 0 from the same broken read).
static uint32_t TrainOrderSeed(void* self)
{
    uint8_t* s = (uint8_t*)self;
    if (!s || !Readable(s + 0x48, 8)) return 0;
    uint8_t* a = *(uint8_t**)(s + 0x48);
    if (!a || !Readable(a + 0x18, 8)) return 0;
    void* arg = *(void**)(a + 0x18);
    if (!arg) return 0;
    typedef int (*GameTimeGet)(void*);
    return (uint32_t)((GameTimeGet)(g_base + RVA_GAMETIME_GET))(arg);
}

// ---- the three steps of a component read (see the header comment) ----------

// 1. The Name component's type index in this world. Called once per sim step,
// never cached: a type index belongs to a world, and a new game or a loaded
// save is a new world at an address the old one may well have been freed from.
// One map lookup a step is not worth the risk of a stale index silently reading
// a different component type.
static int TrainOrderNameType(uint8_t* world)
{
    const void* ti = (const void*)(g_base + RVA_TI_NAME);
    typedef int (*TypeIndexFn)(void*, const void**);
    return ((TypeIndexFn)(g_base + RVA_ECS_TYPEINDEX))(world + 0x48, &ti);
}

// 2. The slot this entity's component of that type sits in, or -1 when it has
// none. Deliberately NOT 0xd0920: that one aborts the process on a miss.
static int TrainOrderSlot(uint8_t* world, int32_t entity, int typeIdx)
{
    if (entity < 0 || entity > 0x0fffffff) return -1;
    uint8_t* table = *(uint8_t**)(world + 0xa0);
    if (!table) return -1;
    uint8_t* ent = table + (size_t)entity * 24;      // vector<pair<int,int>> per entity
    uint8_t* b = *(uint8_t**)ent;
    uint8_t* e = *(uint8_t**)(ent + 8);
    if (!b || e < b || (size_t)(e - b) % 8 || (size_t)(e - b) > 8 * 4096) return -1;
    for (; b != e; b += 8) {
        int32_t t, slot;
        memcpy(&t, b, 4);
        if (t != typeIdx) continue;
        memcpy(&slot, b + 4, 4);
        return slot;
    }
    return -1;
}

// 3. Where that slot lives. Both branches of the engine's own indexing, the
// flat array and the paged table it switches to at 0x40000000.
static const uint8_t* TrainOrderComponent(uint8_t* world, int typeIdx, int slot)
{
    if (typeIdx < 0 || typeIdx > 4096 || slot < 0) return nullptr;
    uint8_t* pools = *(uint8_t**)(world + 0x88);
    if (!pools) return nullptr;
    uint8_t* pool = *(uint8_t**)(pools + (size_t)typeIdx * 8);
    if (!pool) return nullptr;
    if (slot < 0x40000000) {
        uint8_t* data = *(uint8_t**)(pool + 0x68);
        return data ? data + (size_t)slot * 0x20 : nullptr;
    }
    const int32_t e = slot - 0x40000000;
    uint8_t* pages = *(uint8_t**)(pool + 0x80);
    if (!pages) return nullptr;
    uint8_t* page = *(uint8_t**)(pages + (size_t)(e / 32) * 2 * 8);
    return page ? page + (size_t)(e % 32) * 0x20 : nullptr;
}

// The Name component is one std::string. MSVC layout, the same one the SetName
// capture in this file decodes: size at +0x10, capacity at +0x18, text inline
// at +0 while capacity is under 16, otherwise behind the pointer at +0.
static bool TrainOrderNameText(const uint8_t* comp, const char** text, uint32_t* len)
{
    uint64_t sz = 0, cap = 0;
    memcpy(&sz, comp + 0x10, 8);
    memcpy(&cap, comp + 0x18, 8);
    if (sz > TRAINORDER_NAME_MAX || cap < sz) return false;
    const char* p = (const char*)comp;
    if (cap >= 16) { uint64_t ptr = 0; memcpy(&ptr, comp, 8); p = (const char*)ptr; }
    if (!p) return false;
    *text = p; *len = (uint32_t)sz;
    return true;
}

// Is this call worth a line? Always the first one and any change in the train
// count; otherwise 1 seed in 64, picked by a hash of the seed VALUE -- which
// picks the SAME seeds on every peer whatever the game-time stride between sim
// steps is, so the two logs still line up, and keeps this off the per-step log
// budget. Carries its own "have we been here" state, so call it once per call.
static bool TrainOrderShouldLog(uint32_t seed, int64_t n)
{
    static uint32_t lastSeed = 0;
    static int64_t  lastN = -1;
    const bool first = lastN < 0;
    const bool nMoved = n != lastN;
    uint32_t h = seed * 2654435761u; h ^= h >> 16;
    const bool sample = (h >> 26) == 0;
    const bool show = first || nMoved || (seed != lastSeed && sample);
    lastSeed = seed; lastN = n;
    return show;
}

// Refusals are logged once per reason, not once per sim step.
static void TrainOrderLogRefusal(const char* why, int64_t n, uint32_t seed)
{
    static const char* lastRefused = nullptr;
    InterlockedIncrement(&g_toRefusals);
    if (why == lastRefused) return;
    lastRefused = why;
    Log("[trainorder] refused: %s (n=%lld seed=%u) -- the engine's own order stands\n",
        why, (long long)n, seed);
}

// One key per node-list position. Reused across steps so a busy world does not
// allocate once a step; the name POINTERS in it are into live components and are
// never held past the call.
//
// Update2 runs on the sim thread and its reservation loop is serial -- the
// parallel part is the movement loop further down, at 0xabe7fd -- so this buffer
// has one user. g_toBusy costs two interlocked ops a step to make sure: if that
// reading is ever wrong, the second thread backs out and says so, instead of
// resizing the vector under the first one.
static std::vector<TrainOrderKey> g_toKeys;
static volatile LONG g_toBusy = 0;

// The body of the detour. Everything it needs comes from the relay: the idx
// array the engine just filled with iota, the train count (its rbx), the
// vector's end pointer (its [rbp-0x18], kept only to cross-check the count),
// `this`, and the ECS world (its r13).
static void TrainOrderRank(int32_t* idx, int64_t n, const int32_t* idxEnd,
                           void* self, void* world, uint32_t seed)
{
    InterlockedIncrement(&g_toCalls);
    InterlockedExchange(&g_toLastSeed, (LONG)seed);
    InterlockedExchange64(&g_toLastN, (LONG64)n);

    const uint8_t* recs = nullptr;
    const char* refused = nullptr;
    if (!idxEnd || idxEnd < (const int32_t*)idx || (int64_t)(idxEnd - (const int32_t*)idx) != n)
        refused = "count does not match the index vector";
    else if (!self || !Readable((uint8_t*)self + 8, 8))
        refused = "no node list";
    else {
        // this+8 is the node-list holder; its first field is the record base.
        // (0xabe1cc: mov rdx,[rsi+8]; mov rax,[rdx]; lea r15,[rax+idx*12])
        uint8_t* holder = *(uint8_t**)((uint8_t*)self + 8);
        if (!holder || !Readable(holder, 8)) refused = "no node list";
        else {
            recs = *(const uint8_t**)holder;
            if (!recs || !Readable(recs, (size_t)n * TRAINORDER_REC)) refused = "records unreadable";
        }
    }
    if (refused) { TrainOrderLogRefusal(refused, n, seed); return; }
    if (n < 2) return;

    // Keys: the entity id always, the name when the world looks like one we can
    // read. A world we cannot read costs the names, not the ordering -- ranking
    // by id alone is still a pure function of lockstep state, and still beats
    // the registration order we are replacing.
    if ((int64_t)g_toKeys.size() < n) g_toKeys.resize((size_t)n);
    TrainOrderKey* keys = g_toKeys.data();
    for (int64_t i = 0; i < n; i++) {
        keys[i].name = nullptr; keys[i].len = 0; keys[i].score = 0;
        keys[i].id = TrainOrderRecId(recs, (int32_t)i);
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

    const TrainOrderOutcome o = TrainOrderArrange(idx, n, keys, seed);
    if (o.refused) { TrainOrderLogRefusal(o.refused, n, seed); return; }
    if (o.changed) InterlockedIncrement(&g_toReorders);
    if (!TrainOrderShouldLog(seed, n)) return;
    Log("[trainorder] seed=%u n=%lld named=%lld unnamed=%d reordered=%d ids=%08x%s%s\n",
        seed, (long long)n, (long long)o.named, noName, o.changed ? 1 : 0,
        TrainOrderIdHash(idx, n, recs),
        o.duplicates ? " DUPLICATE-IDS" : "",
        typeIdx >= 0 ? "" : " NO-NAMES(world unreadable)");
}

// Called by TrainOrderRelay. A fault anywhere in here costs the ordering for
// one step, never the game: the engine's array is only ever SWAPPED within
// itself, so however far the sort had got, every train is still in it exactly
// once and the reservation loop still visits each of them.
//
// The busy flag is taken and released here, around the whole body, so a fault
// cannot leave it stuck and switch the patch off for the rest of the session.
extern "C" void TrainOrderFix(int32_t* idx, int64_t n, const int32_t* idxEnd,
                              void* self, void* world)
{
    if (InterlockedCompareExchange(&g_toBusy, 1, 0) != 0) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("[trainorder] re-entered from a second thread -- this step keeps the engine's "
                "order. TrainMoveSystem::Update2 was measured as serial; it is not\n");
        }
        InterlockedIncrement(&g_toRefusals);
        return;
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    __try {
        TrainOrderRank(idx, n, idxEnd, self, world, TrainOrderSeed(self));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool told = false;
        if (!told) { told = true; Log("[trainorder] faulted -- ordering skipped this step, the game is untouched\n"); }
    }
    InterlockedExchange(&g_toBusy, 0);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0) return;
    const LONG us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    if (us > g_toMaxUs) {
        InterlockedExchange(&g_toMaxUs, us);
        // Once, and only when it actually costs something. This runs on the sim
        // thread inside the engine's own update: a millisecond here is a
        // millisecond off every sim step on every peer.
        static bool warned = false;
        if (us > 1000 && !warned) {
            warned = true;
            Log("[trainorder] SLOW: %ld us for %lld trains in one step -- over the 1 ms budget\n",
                us, (long long)g_toLastN);
        }
    }
}

// A mid-function detour with NO return path: the relay ends by jumping to
// 0xabe194 itself. InstallHook is not usable here -- it builds a trampoline out
// of the stolen bytes, and these contain a call rel32 that would then point
// into space (and we are skipping the engine's shuffle, not running it).
static bool PatchJump(uintptr_t at, void* to, int len)
{
    if (len < 14 || len > 32) return false;
    DWORD old = 0;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t patch[32];
    patch[0] = 0xFF; patch[1] = 0x25;                 // jmp [rip+0]
    memset(patch + 2, 0, 4);
    uintptr_t d = (uintptr_t)to;
    memcpy(patch + 6, &d, 8);
    memset(patch + 14, 0xCC, len - 14);
    memcpy((void*)at, patch, len);
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}

// Kill switch: `trainorder=0` in tpf2_menu_flags.txt, the same file (and the
// same dumb prefix match) the menu dll reads its own switches from. Looked up
// next to this dll first, then in the data dir.
static bool FlagsSayNoTrainOrder()
{
    for (int i = 0; i < 2; i++) {
        const char* dir = i == 0 ? g_dllDir : g_dataDir;
        if (!dir[0]) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", dir);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (!f) continue;
        char line[256]; bool off = false;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "trainorder=0", 12)) off = true;
        fclose(f);
        return off;
    }
    return false;
}

static void InstallTrainOrder()
{
    if (FlagsSayNoTrainOrder()) {
        Log("[trainorder] OFF (trainorder=0 in tpf2_menu_flags.txt) -- trains keep the "
            "engine's registration-order shuffle, which two peers can disagree about\n");
        return;
    }
    const uintptr_t at = g_base + RVA_TRAINORDER_HOOK;
    if (!Readable((const void*)at, TRAINORDER_STEAL) ||
        memcmp((const void*)at, TRAINORDER_EXPECT, TRAINORDER_STEAL) != 0) {
        char got[3 * TRAINORDER_STEAL + 1]; got[0] = 0;
        if (Readable((const void*)at, TRAINORDER_STEAL))
            for (int i = 0; i < TRAINORDER_STEAL; i++)
                snprintf(got + i * 3, 4, "%02x ", ((const uint8_t*)at)[i]);
        Log("[trainorder] NOT installed: bytes at rva=%llx are not the sequence measured "
            "on build %lu (got: %s)\n", (unsigned long long)RVA_TRAINORDER_HOOK,
            (unsigned long)GAME_BUILD_NUMBER, got[0] ? got : "unreadable");
        return;
    }
    // The call inside those bytes must really be the GameTime accessor: the
    // rel32 is build-specific, so resolve it rather than trust the byte match.
    int32_t rel = 0;
    memcpy(&rel, (const void*)(at + 9), 4);
    const uintptr_t callTarget = (uintptr_t)((int64_t)at + 13 + rel);
    if (callTarget != g_base + RVA_GAMETIME_GET) {
        Log("[trainorder] NOT installed: the call at rva=%llx resolves to %llx, not the "
            "GameTime accessor %llx\n", (unsigned long long)(RVA_TRAINORDER_HOOK + 8),
            (unsigned long long)(callTarget - g_base), (unsigned long long)RVA_GAMETIME_GET);
        return;
    }
    const uintptr_t resume = g_base + RVA_TRAINORDER_RESUME;
    if (!Readable((const void*)resume, sizeof(TRAINORDER_EXPECT_RESUME)) ||
        memcmp((const void*)resume, TRAINORDER_EXPECT_RESUME, sizeof(TRAINORDER_EXPECT_RESUME)) != 0) {
        Log("[trainorder] NOT installed: rva=%llx is not the pair of reloads the engine "
            "leaves its shuffle on\n", (unsigned long long)RVA_TRAINORDER_RESUME);
        return;
    }
    g_trainOrderResume = resume;
    if (!PatchJump(at, (void*)&TrainOrderRelay, TRAINORDER_STEAL)) {
        Log("[trainorder] NOT installed: could not write the detour at rva=%llx\n",
            (unsigned long long)RVA_TRAINORDER_HOOK);
        return;
    }
    g_trainOrderOn = true;
    Log("[trainorder] installed rva=%llx steal=%d resume=%llx -- track is reserved by "
        "name (case-insensitive) with a seeded jitter of n/%lld, never by node-list order\n",
        (unsigned long long)RVA_TRAINORDER_HOOK, TRAINORDER_STEAL,
        (unsigned long long)RVA_TRAINORDER_RESUME, (long long)TRAIN_ORDER_JITTER_DIV);
}

