// ---- HOT-JOIN ORDER: person decisions see their batches in entity-id order ----
//
// A world that kept running and the same world loaded from its save hold the
// same entities with the same components, but not in the same ECS node-list
// order: a load registers every entity in (reverse topological) id order, a
// running world holds its lists in add / swap-remove history. The person sim
// walks those lists and consumes one random stream in list order, so a host
// that keeps its world at a hot join and a joiner that loads the host's save
// pick different destinations within a few game units of the resume, and the
// people count splits later (measured 2026-09-22 on the native lab pair: the
// first differing person 2-5 game units after release, every time).
//
// Three batches decide it (RE 2026-09-22, build 35924):
//   candidates  destination_util::GetTargetsByLandUse 0x9279f0 copies the
//               PersonCapacity node list into a local vector<Entity>; PickTarget
//               0x928370 accumulates free capacity over it IN THAT ORDER and
//               binary-searches one draw -> the same draw names another
//               building when the list order differs.
//   departures  SimEntityAtBuildingSystem::Update2 0xa7c920 collects the people
//               whose stay ran out in node order and signals
//               NoteAtBuildingPersonsLeave 0xa93610, which draws "recompute the
//               destination?" and the stay durations from ONE time-seeded mt19937
//               in batch order.
//   arrivals    PersonMoveSystem::Update2 0xa59450 collects walk arrivals in node
//               order for NoteWalkPersonsArrived 0xa97820 (one tag-3 mt19937,
//               stay durations U(5,300) in batch order).
//   idle        SimEntityIdleSystem::Update 0xa86760 keeps its pending list
//               (system+0x18) in insertion order -- registration order after a
//               load -- and PathFactory::Compute 0x90f320 seeds one mt19937 per
//               chunk (time + chunk start) and draws per item: each trip's path
//               and mode depend on the person's place in the list. Sorted in
//               place (the outputs are matched back by position in the same call).
// Each batch is sorted ascending by entity id just before the engine reads it,
// so each decision depends on the batch's CONTENT only. The entity ids
// themselves agree (the free-id queue is saved, Engine::Load 0x23df8f0).
//
// Every peer of a session must run this (a draw lands on another building than
// vanilla's): the lobby's exact version gate guarantees it. The native Linux
// build carries the same four sorts (docs/re/hotjoin/order_canon_linux.cpp, sites 0x1502918,
// 0x16f0bcb, 0x16b5dc6, 0x17005cc). Kill switch: hotjoinorder=0 in tpf2_menu_flags.txt --
// on every machine at once, or not at all.
//
// Not covered yet (measured order-sensitive, see docs/HOTJOIN_ORDER.md): the
// town stagger (TownSystem::Update2 by node index), the terminal waiting queues
// (FIFO while running, registration order after a load).

extern "C" uint64_t g_hjResume0 = 0, g_hjResume1 = 0, g_hjResume2 = 0, g_hjResume3 = 0;
extern "C" void HotJoinCandidatesRelay();
extern "C" void HotJoinDeparturesRelay();
extern "C" void HotJoinArrivalsRelay();
extern "C" void HotJoinIdleRelay();

struct HotJoinSite {
    const char* name;
    uintptr_t rva;
    int steal;
    uint8_t expect[10];
    size_t expectLen;
    void (*relay)();
    uint64_t* resume;
};
static const HotJoinSite kHotJoinSites[4] = {
    { "candidates", 0x927df6, 7, { 0xC7, 0x45, 0x87, 0x01, 0x00, 0x00, 0x00, 0xE8 }, 8, HotJoinCandidatesRelay, &g_hjResume0 },
    { "departures", 0xa7c9fd, 5, { 0x48, 0x8D, 0x54, 0x24, 0x28, 0x48, 0x8B, 0x49, 0x10, 0xE8 }, 10, HotJoinDeparturesRelay, &g_hjResume1 },
    { "arrivals",   0xa59928, 5, { 0x48, 0x8D, 0x54, 0x24, 0x68, 0x48, 0x8B, 0x49, 0x10, 0xE8 }, 10, HotJoinArrivalsRelay, &g_hjResume2 },
    { "idle",       0xa867ce, 8, { 0x49, 0x8B, 0x55, 0x20, 0x49, 0x2B, 0x55, 0x18, 0x48, 0xC1 }, 10, HotJoinIdleRelay, &g_hjResume3 },
};
static const int kHotJoinSiteCount = (int)(sizeof(kHotJoinSites) / sizeof(kHotJoinSites[0]));
static volatile LONG64 g_hjCalls[4] = { 0 }, g_hjReordered[4] = { 0 }, g_hjRefused[4] = { 0 }, g_hjFaults[4] = { 0 };

static void HotJoinSortImpl(int site, int32_t** vec)
{
    const LONG64 n = InterlockedIncrement64(&g_hjCalls[site]);
    int32_t* b = vec[0];
    int32_t* e = vec[1];
    int32_t* cap = vec[2];
    if (b > e || e > cap || (b == nullptr) != (e == nullptr) || ((uintptr_t)b & 3) ||
        (size_t)(e - b) > ((size_t)1 << 26) || (e > b && !Readable(b, (size_t)(e - b) * sizeof(int32_t)))) {
        if (InterlockedIncrement64(&g_hjRefused[site]) == 1)
            Log("[hotjoinorder] ERROR: %s: not an entity vector (%p %p %p) -- left as the engine built it\n",
                kHotJoinSites[site].name, (void*)b, (void*)e, (void*)cap);
        return;
    }
    if (!std::is_sorted(b, e)) {
        std::sort(b, e);
        InterlockedIncrement64(&g_hjReordered[site]);
    }
    if (n == 1 || !(n & 0xffff))
        Log("[hotjoinorder] alive: %s calls=%lld reordered=%lld refused=%lld faults=%lld last=%lld\n",
            kHotJoinSites[site].name, (long long)n, (long long)g_hjReordered[site],
            (long long)g_hjRefused[site], (long long)g_hjFaults[site], (long long)(e - b));
}

extern "C" void HotJoinSort(int site, int32_t** vec)
{
    if (site < 0 || site >= kHotJoinSiteCount || !vec) return;
    __try { HotJoinSortImpl(site, vec); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement64(&g_hjFaults[site]); }
}

static void InstallHotJoinOrder()
{
    if (FlagsSayOff("hotjoinorder")) {
        Log("[hotjoinorder] OFF (hotjoinorder=0 in tpf2_menu_flags.txt) -- person batches keep the "
            "engine's node-list order; a retained host and a loaded joiner decide apart\n");
        return;
    }
    for (const auto& s : kHotJoinSites)
        if (!BytesAre(s.rva, s.expect, s.expectLen, "hotjoinorder")) return;
    int on = 0;
    for (const auto& s : kHotJoinSites) {
        *s.resume = g_base + s.rva + (uintptr_t)s.steal;
        if (!PatchJumpNear(g_base + s.rva, (void*)s.relay, s.steal, nullptr)) {
            Log("[hotjoinorder] NOT installed at %s (rva=%llx): could not write the detour -- %d of %d sites "
                "are live, a session with this machine WILL desync at a hot join\n",
                s.name, (unsigned long long)s.rva, on, kHotJoinSiteCount);
            return;
        }
        on++;
    }
    Log("[hotjoinorder] installed: destination candidates, departures, walk arrivals and the idle list "
        "are read in entity-id order (rva=%llx, %llx, %llx, %llx)\n", (unsigned long long)kHotJoinSites[0].rva,
        (unsigned long long)kHotJoinSites[1].rva, (unsigned long long)kHotJoinSites[2].rva,
        (unsigned long long)kHotJoinSites[3].rva);
}
