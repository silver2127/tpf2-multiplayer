// add_hook.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// the CommandList::Add hook: cancel, callbacks, stashes, DeferHandler


// CommandList::Add(list, OUT handle, cmd, ..., callback) writes a handle into
// its second argument, and the caller destroys that handle as soon as Add
// returns. Cancelling the call leaves the caller's stack slot holding whatever
// was there before -- and the destructor (exe+0x2357910) reads *handle, checks
// it against null only, then dereferences handle[1]. A leftover
// 0xfffffffffffffffe passes the null check and faults reading address 6: the
// game crashed on a plane's "turn around" while it was flying to a depot
// (2026-08-30, access violation at exe+0x235791e, rbx = -2). Earlier cancels
// survived only because that slot happened to hold zero. Zeroing the out handle
// makes the caller's destructor a no-op.
static void ZeroAddResult(uint64_t rdx)
{
    if (!rdx) return;
    __try {
        *(volatile uint64_t*)rdx = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[slice] could not zero the Add out-handle at %llx\n", (unsigned long long)rdx);
    }
}

// The construction-placement branch of DeferHandler (caller 419f62), in its own
// function because it holds std::vectors and MSVC forbids objects with
// destructors in a function that uses __try. Street vectors of ANY size: the
// old 64-record node, edge and removal buffers made the decoders CLAMP a
// bigger placement to 64 records, so ROADC shipped a partial street.
static void ConstructionPlacementAtFactory(uint64_t rcx, uint64_t r8)
{
    // One serial per placement capture, stamped on both records this writes:
    // the ROADC companion (ps=) and the CONXP (ps= rc=). The Lua pairs the two
    // by it, so however many polls, stalls or other placements lie between the
    // two reads they still find each other (cons.lua CM.flushConPairs).
    const long ps = ++g_placeSerial;
    const std::vector<Node> cn  = DecodeNodes(r8);
    const std::vector<Edge> ce  = DecodeEdges(r8);
    const std::vector<Edge> crm = DecodeEdges(r8 + 0x30);
    const int n = (int)cn.size(), m = (int)ce.size(), re = (int)crm.size();
    EdgeType cet = DecodeEdgeType(r8);
    if (m >= 1 && cet.ok) {
        g_conroad++;
        Log("[slice] #%ld construction placement: %d street node(s) %d "
            "edge(s) %d removal(s), type=%s streetType=%d -- shipping ROADC ps=%ld\n",
            g_conroad, n, m, re, cet.type == 1 ? "TRACK" : "street",
            cet.streetType, ps);
        WriteInjectConRoad(cn.data(), n, ce.data(), m, crm.data(), re, cet, ps);
        // STRICT LOCKSTEP FOR THE PLACEMENT ITSELF. Walk the params off
        // THIS proposal and stash them; if the Add hook then cancels the
        // native build it ships them as CONXP and the Lua builds the
        // scripted proposal at the stamp on EVERY instance, the originator
        // included -- no native build, no bulldoze, no window.
        // g_pendingNoCb stays 0: the placement is a TOOL and waits on its
        // callback.
        bool stashed = StashConxpFromProposal(r8);
        // rc=1: this placement's street payload IS on the wire, so the Lua
        // waits for it by serial instead of shipping the construction alone.
        if (stashed) { g_conxpSerial = ps; g_conxpHadRoadc = 1; }
        if (stashed && SessionLive()) {
            InterlockedExchange(&g_pendingIsConx, 1);
            InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
            InterlockedExchange(&g_pendingNoCb, 0);
            Log("[slice] armed cancel: construction placement cmd=%llx ps=%ld -- CONXP ships if the cancel lands\n",
                (unsigned long long)rcx, ps);
        } else if (!stashed) {
            Log("[slice] construction placement: params not readable -- NOT cancelled, builds natively (safe fallback)\n");
            if (SessionLive()) WriteNativeNotice("construction");
        }
    } else if (m >= 1) {
        Log("[slice] construction placement has %d street edge(s) but the "
            "type decode failed -- NOT shipping ROADC (peer replica will "
            "stay unconnected)\n", m);
    } else {
        // FREE-STANDING (no street edges: a station away from any road, a
        // harbour, an airport). Nothing to ship as ROADC, but the placement
        // itself is cancelled and replayed like a road-snapped one: the
        // Lua ships the stashed params as CONP cancelled=1 when no ROADC
        // pairs with them, and every instance builds the scripted proposal
        // at the stamp. Until 2026-09-09 this branch built natively and the
        // strict path then bulldozed and rebuilt the station, which is the
        // rebuild that asserted the engine on a modular_station.
        Log("[slice] construction placement carries no street edges "
            "(n=%d) -- free-standing, ps=%ld\n", n, ps);
        bool stashed = StashConxpFromProposal(r8);
        // rc=0: no payload is coming for this one, so the Lua ships it as CONP
        // at once rather than waiting for a ROADC that will never be parked.
        if (stashed) { g_conxpSerial = ps; g_conxpHadRoadc = 0; }
        if (stashed && SessionLive()) {
            InterlockedExchange(&g_pendingIsConx, 1);
            InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
            InterlockedExchange(&g_pendingNoCb, 0);
            Log("[slice] armed cancel: free-standing construction placement cmd=%llx ps=%ld -- CONXP ships if the cancel lands\n",
                (unsigned long long)rcx, ps);
        } else if (!stashed) {
            Log("[slice] free-standing placement: params not readable -- NOT cancelled, builds natively (safe fallback)\n");
            if (SessionLive()) WriteNativeNotice("construction");
        }
    }
}

// rax: 0 = let the original run, 1 = cancel it
// A road/rail build or upgrade proposal (the build tool, the upgrade tool):
// decode it whole, ship it as ROADE and arm the cancel. Its own frame because
// the records live in vectors, which a function holding a __try may not own
// (C2712); DeferHandler wraps the call in the SEH guard as before.
static void CaptureRoadProposal(uint64_t rcx, uint64_t r8, bool isUpgrade)
{
    // Every node and edge the proposal holds. These were arrays of 256 nodes
    // and 512 edges, and DecodeNodes/DecodeEdges CAPPED at them without a word:
    // a long drag shipped its first 512 edges and rebuilt short on every peer.
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    int n = DecodeNodesVec(r8, &nodes, "ROADE addNodes");
    int m = DecodeEdgesVec(r8, &edges, "ROADE addEdges");
    // Gate on EDGES, not new nodes. A road connecting two EXISTING junctions
    // adds ZERO new nodes (both endpoints already exist) and one edge whose
    // node0/node1 are positive existing ids. The old n<2 guard (correct for
    // the all-new-nodes ROADN format, wrong since ROADE) rejected exactly
    // that case: the build was not cancelled, so it happened LOCALLY and
    // never replicated -- observed as a one-edge desync in the two-way test.
    // The ROADE->ROADP converter already resolves positive endpoints to
    // positions via realPos(), so a 0-new-node road rebuilds on the peer.
    if (n < 0 || m < 0) {
        // A vector that does not read (the misread guard, or not whole
        // records): nothing can ship, the build runs natively here, and the
        // peers are told so.
        const bool live = SessionLive();
        Log("[slice] %s capture: proposal vectors unreadable (n=%d m=%d) -- NOT cancelled, "
            "runs natively%s\n", isUpgrade ? "upgrade" : "road", n, m,
            live ? "; NATIVE notice written" : "");
        if (live) WriteNativeNotice(isUpgrade ? "upgrade" : "road");
        return;
    }
    if (m < 1) {
        Log("[slice] %s capture: no edges (n=%d m=%d) -- not a build, "
            "letting it proceed\n", isUpgrade ? "upgrade" : "road", n, m);
        return;
    }
    g_captured++;
    if (isUpgrade)
        Log("[slice] #%ld captured UPGRADE, %d edges replaced\n", g_captured, m);
    else if (n >= 1)
        Log("[slice] #%ld captured road, %d nodes %d edges, first=(%.2f,%.2f) last=(%.2f,%.2f)\n",
            g_captured, n, m, nodes[0].x, nodes[0].y, nodes[n - 1].x, nodes[n - 1].y);
    else
        Log("[slice] #%ld captured road, 0 new nodes %d edges (connects existing junctions)\n",
            g_captured, m);
    // STREET PROPERTY PROBE (log only, upgrades are rare so it is free).
    // A street's bus lane (hasBus) and its tram track (tramTrackType,
    // which also encodes electrification) live in BaseEdgeStreet beside
    // streetType, but DecodeEdgeType never reads them for a street -- it
    // forces trackType and returns. So adding a tram way or a bus lane
    // cannot travel on the wire, and since the upgrade is cancelled and
    // replayed from what IS on the wire, the road came back plain on every
    // instance including the originator (2026-09-03).
    //
    // streetType sits at record +0x4c and trackType at +0x60, so both
    // fields are somewhere in between. Capture this window for one upgrade
    // WITH a tram/bus lane and one without: the byte that differs names the
    // offset. Do NOT hardcode an offset from a single sample.
    // Served its purpose (it named +0x54); keep it for the next unknown
    // street field but off by default -- 120 bytes per upgrade is noise.
    if (isUpgrade && DumpPropOn()) {
        uint64_t pbegin = 0, pend = 0;
        if (Readable((void*)(r8 + 0x18), 16)) {
            memcpy(&pbegin, (void*)(r8 + 0x18), 8);
            memcpy(&pend, (void*)(r8 + 0x20), 8);
            if (pbegin >= 0x10000 && pend > pbegin
                && (pend - pbegin) % 120 == 0 && Readable((void*)pbegin, 120)) {
                const uint8_t* pb = (const uint8_t*)pbegin;
                // WHOLE record. +0x51 was NOT it: it read 239 and 246 on two
                // captures, which is noise rather than a 0/1/2 enum -- and
                // +0x54 tracks streetType (1 for type 19, 2 for type 22), so
                // that is a road property. Dump all 120 bytes and diff a
                // regular-tram upgrade against an electric one on the SAME
                // road type; the byte that differs is tramTrackType. Two
                // guesses were enough.
                char hex[3 * 120 + 8];
                int o = 0;
                for (int i = 0; i < 120 && o + 4 < (int)sizeof(hex); i++)
                    o += snprintf(hex + o, sizeof(hex) - o, "%02x ", pb[i]);
                Log("[slice]   STREETPROBE rec+0x00..0x77: %s\n", hex);
            }
        }
    }
    // Stride-correct removal counts, for the LOG only. removedNodes at
    // r8+0x30 are 24-byte node records and removedSegments at r8+0x48 are
    // 120-byte SegmentAndEntity records (r9_analysis_dem.md 1, DECOMPILED;
    // the old DecodeIds read them at a 4-byte stride, which is how one
    // 120-byte record became "30 removals"). DecodeNodes/DecodeEdges take a
    // base whose vector triplets sit at +0x00/+0x18, so passing r8+0x30
    // addresses exactly the two removal vectors.
    // Read whole, like the adds: a removal list cut short (the 64/512-slot
    // arrays this had) would ship every add against a truncated removal list
    // and the peer would add edges on top of the ones it never removed. An
    // unreadable one reads as -1 and the checks below keep the build local.
    std::vector<Node> rmNodes;
    std::vector<Edge> rmEdges;
    int rn = DecodeNodesVec(r8 + 0x30, &rmNodes, "ROADE rmNodes");
    int re = DecodeEdgesVec(r8 + 0x30, &rmEdges, "ROADE rmEdges");
    EdgeType et = DecodeEdgeType(r8);
    Log("[slice]   type=%s streetType=%d trackType=%d%s\n",
        et.type == 1 ? "TRACK" : "street", et.streetType, et.trackType,
        et.ok ? "" : "  <- DECODE FAILED, falling back to defaults");
    // Topology summary: a junction that splits one road must remove exactly
    // one edge.
    Log("[slice]   removed nodes=%d segs=%d (stride-correct)\n", rn, re);
    for (int i = 0; i < m && i < 12; i++)
        Log("[slice]     edge %d: %d -> %d  btype=%d bidx=%d%s\n", i,
            edges[i].node0, edges[i].node1, edges[i].btype, edges[i].bidx,
            edges[i].btype == 1 ? " (BRIDGE)" : edges[i].btype == 2 ? " (TUNNEL)" : "");

    // Replicating and cancelling are ONE decision: a road built locally AND
    // queued for replay appeared twice on the originating peer. Without a
    // live session the capture is still written (ARMED 0) but nothing is
    // cancelled, so the build runs natively.
    //
    // A FAILED DECODE MUST NOT CANCEL.
    //
    // This is the bug that made it impossible to build more than one road.
    // The validation correctly rejected a bad type decode and printed
    // "DECODE FAILED" -- and then the cancel ran anyway, because et.ok was
    // logged but never tested. The player's build was killed locally and a
    // garbage trackType was queued for replay, so the road vanished and
    // nothing replaced it.
    //
    // "Never cancel on an error" was already the rule in the fault handler
    // below. It just was not applied to the case where the code works fine
    // and the DATA is unusable, which is the more likely failure by far.
    //
    // An UPGRADE with no decodable removals is the same class of failure.
    // It replaces edges in place, so the adds are only half the command:
    // shipping them alone would lay a second edge over every upgraded one on
    // the peer, and cancelling would delete the player's upgrade locally to
    // buy that. Empty removal list -> not usable, so it stays local too.
    // Each stays-local branch also writes the NATIVE notice in a live session:
    // the build happens here and nowhere else, and the peers' logs must say so
    // rather than nothing.
    if (!et.ok) {
        Log("[slice]   NOT cancelling: type decode failed, so this build "
            "cannot be replicated faithfully -- it stays local\n");
        if (SessionLive()) WriteNativeNotice(isUpgrade ? "upgrade" : "road");
    } else if (isUpgrade && re < 1) {
        Log("[slice]   NOT cancelling: upgrade with %d added edge(s) decoded "
            "%d removals -- replaying the adds alone would duplicate every "
            "edge on the peer, so it stays local\n", m, re);
        if (SessionLive()) WriteNativeNotice("upgrade");
    } else if (isUpgrade && re < m) {
        // Fewer removals than adds means the peer would ADD edges over ones it
        // never removed (a shape we have not seen; the decode no longer caps).
        // Never cancel on data we cannot replay faithfully -- the same rule as
        // a failed type decode.
        Log("[slice]   NOT cancelling: upgrade has %d add(s) but only %d "
            "removal(s) -- would duplicate edges on the peer, stays local\n", m, re);
        if (SessionLive()) WriteNativeNotice("upgrade");
    } else {
        // Removed edges travel for the UPGRADE path only. The road tool's
        // splits are still shipped as re=0 and re-derived on each peer
        // (execPolyline splits its own copy); turning that on here would
        // change a working channel's behaviour in the same commit that adds
        // a new one, and a removal the peer cannot match now SKIPS the whole
        // command. Flip it once upgrades have proven the matcher.
        //
        // EXCEPT an edge REPLACED IN PLACE (2026-09-12). A road built under a
        // bridge makes the engine remove that bridge span and add it again between
        // the SAME two existing nodes (capture: removed segs=1, added
        // 111672 -> 111711 btype=1). No peer can re-derive that from positions, so
        // with re=0 every instance laid a second span over the old one and the
        // engine refused the whole build (critical, no collision) -- the road could
        // never be built under a bridge. Such a removal -- both ends existing nodes,
        // and an added edge joining exactly that pair -- now travels; a split
        // parent never has an added edge between its own two ends.
        const Edge* shipRm = rmEdges.data();
        int shipRe = isUpgrade ? re : 0;
        std::vector<Edge> inPlace;   // every in-place replacement, not the first 512
        if (!isUpgrade) {
            for (int i = 0; i < re; i++) {
                const Edge& r = rmEdges[(size_t)i];
                if (r.node0 < 0 || r.node1 < 0) continue;
                for (int j = 0; j < m; j++) {
                    const Edge& a = edges[(size_t)j];
                    if ((a.node0 == r.node0 && a.node1 == r.node1) || (a.node0 == r.node1 && a.node1 == r.node0)) {
                        inPlace.push_back(r);
                        break;
                    }
                }
            }
            if (!inPlace.empty()) {
                Log("[slice]   %d removal(s) replaced in place (e.g. a bridge span over the new road) -- shipped with the build\n", (int)inPlace.size());
                shipRm = inPlace.data();
                shipRe = (int)inPlace.size();
            }
        }
        const bool live = SessionLive();
        WriteArmed(live);
        WriteInject(nodes.data(), n, edges.data(), m, nullptr, 0, shipRm, shipRe, et);
        if (isUpgrade && re > m)
            Log("[slice]   upgrade ships %d add(s) against %d removal(s) -- "
                "more removals than adds, watch the peer\n", m, re);
        // Arm the cancel. The Add hook matches on the COMMAND POINTER, not
        // on a caller RVA, so the upgrade tool's own CommandList::Add call
        // site is recognised with no extra constant -- and its completion
        // callback is fired there like the build tool's (g_pendingNoCb is
        // cleared: this tool waits on the callback, so swallowing it would wedge
        // the upgrade cursor for the rest of the session).
        if (live) {
            InterlockedExchange(&g_pendingNoCb, 0);
            InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
        } else {
            Log("[slice] no live session (mod off, or nobody to replay it) -- the build runs natively\n");
        }
    }
}

extern "C" uint64_t DeferHandler(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9,
                                 uint64_t id, uint64_t retAddr, uint64_t calleeRsp)
{

    // STREET/TRACK TOOL PROPOSAL DUMP (2026-08-29). The road path's BuildProposal
    // (caller 0x459eb7 = StreetBuilder::UpdateEngine) is observed on the
    // CommandList::Add hook and returns from the cancel block ABOVE the id-0
    // dump path, so a dump placed there never fired. Do it first, unconditionally
    // on caller, so a native rail-over-road crossing's exact proposal is
    // recorded (D8m_/D9m_ in this log; decode with tools/dumpprop_vecs.py).
    // Gate on id ONLY. Two native builds with a caller-gated dump never fired:
    // the factory hook does not see 0x459eb7 as its return address (that RVA is
    // what the CommandList::Add hook observes). Dump every BuildProposal and log
    // the real caller so it can be matched by timing/coordinates instead.
    if (id == ID_BUILDPROPOSAL && DumpPropOn()) {
        Log("[slice] DUMPPROP(any) caller_rva=%llx\n", (unsigned long long)(retAddr - g_base));
        DumpProposal(3, r8, r9);
    }
    (void)rdx;
    uint64_t caller = retAddr - g_base;

    if (id == ID_CMDADD) {
        TryFireSpareLine();   // a held create callback waiting for the editor to list its spare line
        if (g_terrainHeldTool) {
            uint64_t carrier = (uint64_t)InterlockedCompareExchange64(&g_terrainCarrierCmd, 0, 0);
            if (carrier && r8 == carrier) {
                InterlockedExchange64(&g_terrainCarrierCmd, 0);
                Log("[terrain] our replay carrier was added %llu ms into the hold -- it applies a step later; waiting for its completion marker\n", (unsigned long long)(GetTickCount64() - g_terrainHeldAt));
            }
            else if (GetTickCount64() - g_terrainHeldAt > TERRAIN_HOLD_MAX_MS) ReleaseTerrainTool("timeout -- no completion marker arrived");
        }
        // Our claimed createLine replay: give its Add the line editor's held callback.
        // Matched by pointer when the command reaches Add as built, else as the first
        // Add from sendCommand's call site on the claiming thread (see g_lcCarrierTid).
        {
            const uint64_t lc = (uint64_t)InterlockedCompareExchange64(&g_lcCarrierCmd, 0, 0);
            const bool viaScript = lc && caller == CALLER_SCRIPT_SENDCOMMAND
                                   && (DWORD)InterlockedCompareExchange(&g_lcCarrierTid, 0, 0) == GetCurrentThreadId();
            if (lc && (r8 == lc || viaScript)) {
                InterlockedExchange64(&g_lcCarrierCmd, 0);
                if (r8 != lc) Log("[slice] CreateLine: our replay reached Add as %llx (built as %llx) from sendCommand\n", (unsigned long long)r8, (unsigned long long)lc);
                __try { SwapInLineCreateCallback(r9, calleeRsp); }
                __except (EXCEPTION_EXECUTE_HANDLER) { Log("[slice] CreateLine: swap fault -- the replay runs with the Lua's callback\n"); }
                return 0;
            }
        }
        // Pointer match first: this runs ~100/sec and almost never matches.
        uint64_t want = (uint64_t)InterlockedCompareExchange64(&g_pendingCmd, 0, 0);
        if (!want || r8 != want) return 0;
        g_addSeen++;
        InterlockedExchange64(&g_pendingCmd, 0);
        {
            // FIRE THE COMPLETION CALLBACK BEFORE SUPPRESSING.
            //
            // CommandList::Add's 4th argument (r9) is the UI's completion
            // callback, and the build tool WAITS on it. Swallowing the call
            // swallowed the callback, so the tool hung forever -- one cancel and
            // that tool was dead for the rest of the session. Suppressing a
            // function whose contract is "I will call you back" without
            // honouring that contract is the bug, not the cancel itself.
            //
            // Layout verified against the binary, not assumed. UpdateEngine
            // builds the callback at [rsp+0x78] as { vftable*, captured this },
            // so r9 points straight at the impl and vftable slots are:
            //   0,1 _Copy/_Move   (copies vftable + one qword -> 16-byte impl)
            //   2   _Do_call
            //   3   _Target_type  (lea rax,[rip+X]; ret -- 2 instructions)
            //   4   _Delete_this  (frees 0x10 bytes -- confirms the 16 bytes)
            // A two-instruction RTTI getter can only be _Target_type, and
            // _Delete_this freeing exactly the size _Copy implies pins the
            // order. Guessing this slot would crash inside the UI thread.
            //
            // _Do_call(this, Command const&) -> rcx = r9, rdx = the Command,
            // which is r8 at this call site.
            // FIRE-AND-FORGET FIRST. SetLine (6) and Reverse (10) are armed
            // with g_pendingNoCb=1: nothing waits on their callback, and
            // FIRING it here with the command's success byte still 0 makes the
            // UI take its FAILURE branch -- SetLine then pops "unable to find a
            // path to a stop", a false alarm since the Lua replays the
            // assignment at the stamp on every instance (review, 2026-09-01).
            // So suppress WITHOUT firing. Only the build/upgrade tools
            // (g_pendingNoCb==0) fall through to fire their callback, which
            // they DO wait on (cancel-at-commandlist-add-wedges-the-ui).
            if (InterlockedCompareExchange(&g_pendingNoCb, 0, 0)) {
                InterlockedExchange(&g_pendingNoCb, 0);
                InterlockedExchange(&g_pendingHonour, 0);
                if (InterlockedExchange(&g_pendingStashCb, 0)) {
                    const int32_t spare = (int32_t)InterlockedExchange(&g_lcSpareId, 0);
                    bool held = false;
                    __try { held = StashLineCreateCallback(r9); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { held = false; }
                    if (held && spare) {
                        // fired from a later Add on this thread, once the editor lists the spare (TryFireSpareLine)
                        g_lcUiTid = GetCurrentThreadId();
                        g_lcSpareWaitAt = GetTickCount64();
                        InterlockedExchange(&g_lcSpareWaitId, spare);
                        Log("[slice] CreateLine: the line editor's callback is held to open spare line %d as soon as the editor lists it\n", spare);
                    } else {
                        Log(held ? "[slice] CreateLine: the line editor's callback is held for our replay at the stamp\n"
                                 : "[slice] CreateLine: callback could not be held -- cancelled anyway (ARMED 1 promised the replay); the editor will not select the new line\n");
                    }
                }
                g_suppressed++;
                ZeroAddResult(rdx);
                Log("[slice] CANCEL fire-and-forget (caller_rva=%llx), callback "
                    "NOT fired -- avoids the false no-path toast\n", (unsigned long long)caller);
                if (InterlockedExchange(&g_pendingIsConx, 0)) WriteInjectConxp();
                if (InterlockedExchange(&g_pendingIsConu, 0)) WriteInjectConup();
                if (InterlockedExchange(&g_pendingIsStop, 0)) WriteInjectStop();
                if (InterlockedExchange(&g_pendingIsStopDel, 0)) WriteInjectStopDel();
                if (InterlockedExchange(&g_pendingIsTerrain, 0)) WriteInjectTerrain(true); if (InterlockedExchange(&g_pendingIsAssets, 0)) WriteInjectAssets(true);
                return 1;
            }
            bool fired = false;
            // r9 is the std::function OBJECT, not its impl. MSVC keeps the impl
            // pointer in _Mystorage._Ptrs[7] = r9+0x38 (_Getimpl): for a small
            // functor it points back INTO the object (== r9, which is why
            // reading *(r9) worked for the build tool), for a large one at a
            // heap block -- BuyVehicle's and ReplaceVehicle's, whose vftable sat
            // in the unused small buffer and made every fire fail (74fda9).
            // A zero slot is an empty function: nothing to fire.
            uint64_t impl = r9;
            if (Readable((void*)(r9 + 0x38), 8)) {
                uint64_t p = 0;
                memcpy(&p, (void*)(r9 + 0x38), 8);
                if (p && Readable((void*)p, 8)) impl = p;
            }
            if (impl != r9)
                Log("[slice] callback impl is heap-allocated (%llx, function object %llx)\n",
                    (unsigned long long)impl, (unsigned long long)r9);
            // The buy callback (buy_cb_body 0x748250) reads the result vehicle
            // entity at command+0x38 and touches the depot window only when it
            // is not -1. Log what it holds at Add so the "fire is safe" premise
            // is measured, not assumed; if it turns out non -1 the fix is to
            // write -1 there before firing (the command is ours, never applied).
            if (Readable((void*)(r8 + 0x38), 4)) {
                int32_t resEnt = 0;
                memcpy(&resEnt, (void*)(r8 + 0x38), 4);
                Log("[slice] command+0x38 (result entity slot) = %d before the fire\n", resEnt);
            }
            if (Readable((void*)impl, 8) && Readable((void*)r8, 8)) {
                uint64_t vft = 0;
                memcpy(&vft, (void*)impl, 8);
                if (vft && Readable((void*)vft, 8 * 5)) {
                    uint64_t doCall = 0;
                    memcpy(&doCall, (void*)(vft + 0x10), 8);
                    if (doCall && doCall == (uint64_t)g_base + RVA_BUY_CALLBACK_THUNK) {
                        // 0x748250 reads the result vehicle at (*command)+0x38 -- the command
                        // IMPL, after checking its type tag at +0xb18 (13, BuyVehicle) -- not at
                        // command+0x38 as logged above. Never applied, it must say "none" (-1),
                        // or a clone would SetLine whatever entity the slot happens to name.
                        int32_t cloneLine = -1, resVeh = -1;
                        __try {
                            uint64_t cimpl = 0;
                            if (Readable((void*)(impl + 0x38), 4)) memcpy(&cloneLine, (void*)(impl + 0x38), 4);
                            if (Readable((void*)r8, 8)) memcpy(&cimpl, (void*)r8, 8);
                            if (cimpl && Readable((void*)(cimpl + 0x38), 4)) {
                                memcpy(&resVeh, (void*)(cimpl + 0x38), 4);
                                if (resVeh != -1) { const int32_t none = -1; memcpy((void*)(cimpl + 0x38), &none, 4); }
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) { cloneLine = -1; }
                        Log("[slice] buy callback: lambda line=%d (>= 0: a clone), impl result vehicle=%d%s\n",
                            cloneLine, resVeh, resVeh != -1 ? " -- reset to -1 before the fire" : "");
                        if (cloneLine >= 0) WriteInjectBuyLine(cloneLine);
                    }
                    if (doCall) {
                        __try {
                            ((void (*)(uint64_t, uint64_t))doCall)(impl, r8);
                            fired = true;
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            fired = false;
                        }
                        // a cancelled terrain edit: keep the stroke waiting for our replay
                        if (fired && (InterlockedCompareExchange(&g_pendingIsTerrain, 0, 0) || InterlockedCompareExchange(&g_pendingIsAssets, 0, 0))) HoldTerrainTool(impl);
                    }
                }
            }
            if (!fired) {
                // Could not tell the UI the command finished. Cancelling now
                // would wedge the tool exactly as before, so let the build run
                // instead: a local build that also replicates is a visible,
                // recoverable desync; a dead build tool is not.
                if (InterlockedExchange(&g_pendingNoCb, 0)) {
                    // Fire-and-forget command (vehicle/line): nothing waits on
                    // the callback, so suppress cleanly. This is what makes the
                    // originator apply at the STAMP instead of at click time.
                    g_suppressed++;
                    ZeroAddResult(rdx);
                    Log("[slice] CANCEL fire-and-forget (caller_rva=%llx), no callback "
                        "needed -- now owned by lockstep\n", (unsigned long long)caller);
                    if (InterlockedExchange(&g_pendingIsConx, 0)) WriteInjectConxp();
                    if (InterlockedExchange(&g_pendingIsConu, 0)) WriteInjectConup();
                    if (InterlockedExchange(&g_pendingIsStop, 0)) WriteInjectStop();
                    if (InterlockedExchange(&g_pendingIsStopDel, 0)) WriteInjectStopDel();
                    if (InterlockedExchange(&g_pendingIsTerrain, 0)) WriteInjectTerrain(true); if (InterlockedExchange(&g_pendingIsAssets, 0)) WriteInjectAssets(true);
                    return 1;
                }
                if (InterlockedExchange(&g_pendingHonour, 0)) {
                    // ARMED 1 is already on disk: the Lua WILL replay this on
                    // the originator. Running it natively as well is the
                    // double-apply of 7a29978. Honour the cancel; the window
                    // that wanted the callback refreshes from the replay.
                    g_suppressed++;
                    ZeroAddResult(rdx);
                    Log("[slice] callback NOT fired but the cancel is ARMED -- honouring it "
                        "(caller_rva=%llx); the UI did not get its completion, refresh the window if it looks stale\n",
                        (unsigned long long)caller);
                    return 1;
                }
                // Each of these ran natively after all: a NATIVE notice gets it to the
                // mod's catch-up scan (nothing scans the world on a timer any more).
                if (InterlockedExchange(&g_pendingIsConx, 0)) {
                    Log("[slice] construction cancel did not land -- CONXP dropped, the catch-up scan captures the native build\n");
                    WriteNativeNotice("construction");
                }
                if (InterlockedExchange(&g_pendingIsConu, 0)) {
                    Log("[slice] upgrade cancel did not land -- CONUP dropped, the catch-up scan captures the native upgrade\n");
                    WriteNativeNotice("upgrade");
                }
                if (InterlockedExchange(&g_pendingIsStop, 0)) {
                    Log("[slice] stop cancel did not land -- STOPX dropped, the catch-up scan captures the native build\n");
                    WriteNativeNotice("stop");
                }
                if (InterlockedExchange(&g_pendingIsStopDel, 0)) {
                    Log("[slice] stop bulldoze cancel did not land -- STOPXDEL dropped, the catch-up scan ships the removal\n");
                    WriteNativeNotice("stop");
                }
                if (InterlockedExchange(&g_pendingIsTerrain, 0)) {
                    Log("[slice] terrain cancel did not land -- the edit ran natively here; shipping it for the peers behind ARMED 0\n");
                    WriteInjectTerrain(false);
                }
                if (InterlockedExchange(&g_pendingIsAssets, 0)) {
                    Log("[slice] asset stroke cancel did not land -- the stroke ran natively here; shipping it for the peers behind ARMED 0\n");
                    WriteInjectAssets(false);
                }
                Log("[slice] callback NOT fired -- letting the build run rather "
                    "than wedging the tool (caller_rva=%llx)\n",
                    (unsigned long long)caller);
                return 0;
            }
            InterlockedExchange(&g_pendingNoCb, 0);
            InterlockedExchange(&g_pendingHonour, 0);
            g_suppressed++;
            ZeroAddResult(rdx);
            Log("[slice] CANCEL local build (caller_rva=%llx), completion callback "
                "fired -- now owned by lockstep\n", (unsigned long long)caller);
            if (InterlockedExchange(&g_pendingIsConx, 0)) WriteInjectConxp();
            if (InterlockedExchange(&g_pendingIsConu, 0)) WriteInjectConup();   // the cancel LANDED
            if (InterlockedExchange(&g_pendingIsStop, 0)) WriteInjectStop();    // the cancel LANDED
            if (InterlockedExchange(&g_pendingIsStopDel, 0)) WriteInjectStopDel(); // the cancel LANDED
            if (InterlockedExchange(&g_pendingIsTerrain, 0)) WriteInjectTerrain(true); if (InterlockedExchange(&g_pendingIsAssets, 0)) WriteInjectAssets(true); // the cancel LANDED
            return 1;
        }
    }

    if (id == ID_SETGAMESPEED) {
        __try {
            CaptureSpeedButton(rcx, rdx, caller);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[slice] speed button capture fault -- the click runs natively\n");
        }
        return 0;
    }

    if (id == ID_SETDATE || id == ID_SETCALENDARSPEED) {
        __try {
            CaptureCalendar(id, rcx, rdx, caller);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[slice] calendar capture fault -- the change runs natively\n");
        }
        return 0;
    }

    if ((id >= 2 && id <= 10) || id == 13 || id == 14) {
        const Factory* f = nullptr;
        for (int i = 0; i < NUM_FACTORIES; i++) if (FACTORIES[i].id == (int)id) f = &FACTORIES[i];
        if (!f) return 0;
        // STRICT LOCKSTEP: cancel the UI-issued command so the originator
        // applies it at the SAME game-time stamp as every peer, not
        // optimistically at click time. NEVER cancel our own Lua-path replay
        // (scripting block 0xcec000..0xcf2000) -- that would cancel the replay
        // we just issued. The Lua replays on the originator only when ARMED
        // says the cancel happened.
        //
        // Cancelled, fire-and-forget (nothing in the UI waits on a result):
        //   Reverse (10)     -- verified live, suppresses cleanly.
        //   SetLine (6)      -- assigned at click time, a train kept a 0.8 s
        //                       departure offset for the rest of the game
        //                       (measured 2026-08-31).
        //   SellVehicle (3), SendToDepot (5) -- the sell refund moved the
        //                       originator's balance at click time and the
        //                       peers' at the stamp, a coop money-split source.
        //   UpdateLine (8), DeleteLine (9) -- the new stop list is decoded off
        //                       the command (DecodeLine); CaptureFactory clears
        //                       `cancel` when that decode fails.
        // Cancelled, callback fired first (the window WAITS on the result
        // entity -- waitsForResult in CaptureFactory):
        //   BuyVehicle (2), ReplaceVehicle (4). The callback is a heap-allocated
        //   std::function the Add hook resolves through r9+0x38; if a fire still
        //   fails the armed cancel is honoured anyway (g_pendingHonour) rather
        //   than run on top of the replay -- the two-vehicles-for-one-click bug
        //   of 7a29978.
        // CaptureFactory drops the cancel for any of these whose payload does
        // not read or does not reach the inject file.
        // Never cancelled:
        //   SetColor (13), SetName (14) -- shipped only.
        // Cancelled, callback MOVED to our replay (STRICT LINE CREATION above):
        //   CreateLine (7) from line_util only -- firing its callback on a cancelled
        //                       create is a fatal assert, so it rides on the
        //                       originator's own replay at the stamp instead.
        const bool luaPath = IsScriptCaller(caller);
        const bool strictId = (id == 2 || id == 3 || id == 4 || id == 5 ||
                               id == 6 || id == 8 || id == 9 || id == 10) ||
                              (id == 7 && caller == CALLER_UI_CREATELINE);
        bool cancel = !luaPath && strictId;
        __try {
            CaptureFactory(*f, rcx, rdx, r8, r9, calleeRsp, caller, cancel);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[slice] capture fault in %s -- proceeding\n", f->name);
        }
        return 0;
    }

    if (id != ID_BUILDPROPOSAL) return 0;

    // TERRAIN TOOLS. A terraform, paint or asset-brush commit is logged (and
    // saved with dumpprop). In a live session a terraform is cancelled and
    // shipped as TERRAINCAP, a paint stroke runs natively and is shipped, and an
    // asset-brush stroke is cancelled and shipped as ASSETCAP.
    if (caller == CALLER_PROPOSALACTION) {
        bool stashed = false;
        __try {
            stashed = LogTerrainProposal(r8, r9);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[terrain] decode fault -- the edit runs natively, nothing shipped\n");
        }
        if (stashed && g_terrainIsPaint) {
            // PAINT IS NOT CANCELLED (the user's call, 2026-09-11). It moves no
            // heights, so nothing built later reads a different terrain while the
            // peers catch up, and the painter commits only on release -- no
            // mid-stroke part to wait for. It paints here at once and ships behind
            // ARMED 0: every other instance applies it at the stamp.
            Log("[terrain] #%ld paint: runs natively here, shipped for the peers\n", g_terrainStashSeq);
            WriteInjectTerrain(false);
        } else if (stashed) {
            // STRICT: cancel the originator's own commit and let every instance
            // apply the grids at the stamp. A UI tool: it waits on its completion
            // callback, so g_pendingNoCb stays 0 and the Add hook fires it, as for
            // the build tool. No live session -> the edit runs natively here
            // and ships behind ARMED 0 for the peers.
            if (SessionLive()) {
                InterlockedExchange(&g_pendingIsTerrain, 1);
                InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
                InterlockedExchange(&g_pendingNoCb, 0);
                Log("[slice] armed cancel: terrain edit cmd=%llx -- TERRAINCAP ships from the Add hook\n", (unsigned long long)rcx);
            } else {
                Log("[slice] terrain edit with no live session -- runs natively here, shipped for the peers\n");
                WriteInjectTerrain(false);
            }
        }
        // THE ASSET BRUSH, STRICT like terraform: cancelled here so the groups it
        // removes still stand while the mod turns their ids into positions, and
        // every instance applies the stroke at the stamp.
        bool astashed = false;
        if (!stashed) {
            __try {
                astashed = StashAssetsFromProposal(r8, g_terrainSeq);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[asset] decode fault -- the stroke runs natively, nothing shipped\n");
            }
        }
        if (astashed) {
            if (SessionLive()) {
                InterlockedExchange(&g_pendingIsAssets, 1);
                InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
                InterlockedExchange(&g_pendingNoCb, 0);
                Log("[slice] armed cancel: asset stroke cmd=%llx -- ASSETCAP ships from the Add hook\n", (unsigned long long)rcx);
            } else {
                // solo: nothing to replay; drop the stash, the engine builds it
                free(g_assetB64); g_assetB64 = nullptr;
            }
        }
        return 0;
    }

    // Differential proposal dump (cfg 'dumpprop'): UI placement vs Lua replay.
    // dumpprop covers construction placements (0x419f62 UI, 0xced378 Lua) and,
    // as of 2026-08-29, the STREET/TRACK tool (0x459eb7) too: a native rail-over-
    // road crossing is only ever built by that tool, and its exact proposal
    // (which segments/nodes the UI submits at the crossing node) is the ground
    // truth the Lua replay has been unable to reproduce ("Collision").
    if ((caller == 0x419f62 || caller == 0xced378 || caller == 0x459eb7) && DumpPropOn())
        DumpProposal(caller == 0x419f62 ? 1 : (caller == 0x459eb7 ? 3 : 2), r8, r9);

    // A Lua-issued construction proposal (our CONX replay): merge our shipped
    // apron INTO the template's connector so the engine sees the UI's shape.
    if (caller == 0xced378) {
        __try {
            if (InjectTerrainFromFile(r8))          // our TERRAIN replay carrier (inert without its file)
                InterlockedExchange64(&g_terrainCarrierCmd, (LONG64)rcx);
            else if (InjectAssetsFromFile(r8))      // our ASSETS replay carrier (inert without its file)
                InterlockedExchange64(&g_terrainCarrierCmd, (LONG64)rcx);
            else if (g_terrainHeldTool && ProposalIsEmpty(r8))
                ReleaseTerrainTool("the replay carrier completed (its marker proposal arrived)");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[terrain-inject] fault -- proposal left as built\n");
        }
        __try {
            bool merged = MergeTemplateStreet(r8);
            if (merged && DumpPropOn())
                DumpProposal(3, r8, r9);            // post-merge, for the diff tool
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[merge] fault -- proposal left as built\n");
        }
    }

    // Bulldozer path (UI::Bulldozer::Apply): classify, ship what decodes, and
    // arm the cancel. Checked before the road path so a bulldoze can never be
    // mistaken for a road capture.
    if (caller == CALLER_BULLDOZE) {
        bool shipped = LogBulldoze(r8);
        // STRICT LOCKSTEP, same shape as the build and upgrade tools: cancel
        // the player's own bulldoze and let the Lua replay it at the agreed
        // stamp, so every instance removes the road at the SAME game-time
        // instead of the originator removing it at click time and the peers
        // some fraction of a second later.
        //
        // g_pendingNoCb stays 0 deliberately. The bulldozer is a TOOL and it
        // WAITS on its completion callback, so it must be fired at Add exactly
        // as the build tool's is; swallowing it wedges the cursor for the rest
        // of the session (cancel-at-commandlist-add-wedges-the-ui). If the fire
        // fails the Add hook lets the bulldoze run rather than wedging the
        // tool, and the replay simply finds the road already gone -- the same
        // optimistic behaviour as before, not a new failure.
        //
        // Only armed when something was actually SHIPPED. Cancelling a bulldoze
        // whose payload never reached the wire would delete the road on nobody:
        // the player's own removal suppressed, no command to replay it.
        if (shipped) {
            if (SessionLive()) {
                InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
                InterlockedExchange(&g_pendingNoCb, 0);
                Log("[slice] armed cancel: bulldoze cmd=%llx -- now owned by "
                    "lockstep, replays at the stamp\n", (unsigned long long)rcx);
            } else {
                Log("[slice] bulldoze shipped but not cancelled (no live session) "
                    "-- it runs natively here and replays on the peers\n");
                // a stash that was never armed must not ride the next landed cancel
                InterlockedExchange(&g_pendingIsStopDel, 0);
                InterlockedExchange(&g_pendingIsConu, 0);
            }
        }
        return 0;
    }

    // Construction placement (caller 419f62): ship its street vectors as a
    // ROADC companion so the peer can weld the replica into its road network,
    // and cancel the placement itself when its params walk (CONXP, below). If
    // the params do not walk or no session is live, the native build stands.
    if (caller == 0x419f62) {
        __try {
            ConstructionPlacementAtFactory(rcx, r8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[slice] ROADC decode fault -- placement proceeds, nothing shipped\n");
        }
        return 0;
    }

    // The street/track UPGRADE tool takes the SAME path as the builder from here
    // on: same proposal struct, same decoders, same cancel-and-replay. Its shape
    // is the only difference (0 new nodes, N adds, N removals), and the branches
    // below say so where it matters.
    // STOP / SIGNAL / WAYPOINT tool: strict cancel-and-replay.
    // Decode the edge-object record and the rebuilt edge off THIS proposal,
    // arm the cancel (a UI TOOL: it waits on its callback, so g_pendingNoCb=0
    // fires it exactly as the road tool's is), and let the Add hook write STOPX
    // only when the cancel lands. Undecodable -> not cancelled, builds natively
    // and the poll replicates it as before (never cancel on a failed decode).
    if (caller == CALLER_STOPTOOL) {
        bool stashed = false;
        __try { stashed = StashStopFromProposal(r8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { stashed = false; }
        if (stashed && SessionLive()) {
            InterlockedExchange(&g_pendingIsStop, 1);
            InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
            InterlockedExchange(&g_pendingNoCb, 0);
            Log("[slice] armed cancel: stop/signal tool cmd=%llx -- STOPX ships if the cancel lands\n",
                (unsigned long long)rcx);
        } else {
            const bool live = SessionLive();
            Log("[slice] stop tool: %s -- NOT cancelled, builds natively%s\n",
                stashed ? "no live session" : "record not decodable",
                live ? " (the mod's catch-up scan replicates it)" : "");
            if (live) WriteNativeNotice("stop");
        }
        return 0;
    }
    const bool isUpgrade = (caller == CALLER_UPGRADE || caller == CALLER_BRIDGE_UPGRADE);

    if (caller != CALLER_BUILDPROPOSAL && !isUpgrade) {
        // Log and move on. The previous version returned here in silence, so a
        // player reporting "I can't build anything" left NO trace at all -- there
        // was no way to tell a station attempt from a bulldoze from nothing
        // happening. A build is a rare event; logging every one costs nothing.
        // ...and say WHAT was ignored, not just that something was. A caller
        // we do not handle is a player action that does not replicate, so the
        // log has to carry enough shape to identify the tool without a
        // debugger. The Lua range is our own replay and is expected; anything
        // else is a real UI path going unreplicated, and the counts name it:
        // adds and removals with NO new nodes is an in-place edit (a crossing
        // upgraded to a double slip switch, a bridge type swapped, a level
        // crossing changed), which is exactly the class the wiki describes as
        // "select it with the inspector and say yes".
        const bool luaReplay = IsScriptCaller(caller);
        if (luaReplay) {
            Log("[slice] BuildProposal from caller_rva=%llx (our own Lua replay) -- ignored\n",
                (unsigned long long)caller);
        } else if (IsUpgradeShape(r8)) {
            // A construction upgrade from a caller we never recorded (the
            // module builder adding/removing a module, a station upgrade):
            // detected by SHAPE, not RVA, so the caller is logged for the
            // record. Same strict route as the bulldozer's module removal.
            if (StashConupFromProposal(r8) && SessionLive()) {
                InterlockedExchange(&g_pendingIsConu, 1);
                InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
                InterlockedExchange(&g_pendingNoCb, 0);
                Log("[slice] armed cancel: construction UPGRADE from caller_rva=%llx old=%d -- CONUP ships if the cancel lands\n",
                    (unsigned long long)caller, g_conupOldId);
            } else {
                const bool live = SessionLive();
                Log("[slice] construction UPGRADE from caller_rva=%llx -- runs natively (params %s)%s\n",
                    (unsigned long long)caller,
                    !g_conxpParams.empty() ? "readable" : "not readable",
                    live ? "; the mod's edit scan or catch-up scan ships it" : "");
                if (live) WriteNativeNotice("upgrade");
            }
        } else {
            int an = -1, ae = -1, rn = -1, re = -1;
            __try {
                uint64_t b = 0;
                an = (int)(ReadVecAny(r8 + 0x00, &b, "unreplicated addNodes") / 24);
                ae = (int)(ReadVecAny(r8 + 0x18, &b, "unreplicated addEdges") / 120);
                rn = (int)(ReadVecAny(r8 + 0x30, &b, "unreplicated rmNodes") / 24);
                re = (int)(ReadVecAny(r8 + 0x48, &b, "unreplicated rmEdges") / 120);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                an = ae = rn = re = -1;
            }
            Log("[slice] UNREPLICATED BuildProposal from caller_rva=%llx: "
                "addNodes=%d addEdges=%d rmNodes=%d rmEdges=%d -- a UI path we do not "
                "handle; what the player just did did NOT reach the peers\n",
                (unsigned long long)caller, an, ae, rn, re);
            // NODE DIFFERENTIAL. An in-place node edit REMOVES the node and
            // ADDS it back changed, so one capture already contains its own
            // control: the removed record is the before-state and the added
            // record the after-state, same node, same position, differing only
            // in the property the player just changed. Dumping both is how the
            // double-slip-switch bit gets identified WITHOUT a guess and
            // without a contrived sweep whose sample index correlates with the
            // value being probed.
            //
            // Record layout (docs/re/PROPOSALS.md): 24 bytes,
            // x,y,z at +0x00, flags u32 at +0x0c, type at +0x10, id at +0x14.
            // Log only. Nothing is cancelled and nothing is shipped: with the
            // carrying bit still unknown, replaying this proposal would rebuild
            // the crossing WITHOUT the property and destroy the edit on the
            // instance that made it.
            if (an >= 1 || rn >= 1) {
                __try {
                    uint64_t ab = 0, rb = 0;
                    ReadVecAny(r8 + 0x00, &ab, "unreplicated addNodes");
                    ReadVecAny(r8 + 0x30, &rb, "unreplicated rmNodes");
                    for (int k = 0; k < 2; k++) {
                        uint64_t base = k ? rb : ab;
                        int cnt = k ? rn : an;
                        if (!base || cnt < 1) continue;
                        for (int i = 0; i < cnt && i < 4; i++) {
                            const uint8_t* b2 = (const uint8_t*)base + (size_t)i * 24;
                            if (!Readable((void*)b2, 24)) break;
                            float x, y, z; uint32_t fl; int32_t ty, id2;
                            memcpy(&x, b2 + 0x00, 4); memcpy(&y, b2 + 0x04, 4);
                            memcpy(&z, b2 + 0x08, 4); memcpy(&fl, b2 + 0x0c, 4);
                            memcpy(&ty, b2 + 0x10, 4); memcpy(&id2, b2 + 0x14, 4);
                            char hex[64]; int o2 = 0;
                            for (int j = 0; j < 24 && o2 < (int)sizeof(hex) - 3; j++)
                                o2 += snprintf(hex + o2, sizeof(hex) - o2, "%02x", b2[j]);
                            Log("[slice]   %sNode[%d] pos=(%.2f,%.2f,%.2f) flags=0x%08x type=%d id=%d hex=%s\n",
                                k ? "rm" : "add", i, x, y, z, fl, ty, id2, hex);
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("[slice]   node dump faulted -- ignored\n");
                }
            }
        }
        return 0;
    }

    __try {
        CaptureRoadProposal(rcx, r8, isUpgrade);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[slice] capture faulted -- proceeding, never cancel on an error\n");
        InterlockedExchange64(&g_pendingCmd, 0);
        return 0;
    }
    return 0;
}

