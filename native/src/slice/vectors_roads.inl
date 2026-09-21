// vectors_roads.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// the game's vectors at their own length, road/rail node and edge decoding, the ROADE/EDEMO/CDEMO/ROADC records


// The {begin,end} pair at vecAddr, empty told apart from unreadable: VEC_EMPTY
// for a well-formed empty vector (begin == end), VEC_OK with the readable span
// in bytes, VEC_UNREADABLE (logged under `tag` when the guard trips) otherwise.
static VecRead ReadVecAnyEx(uint64_t vecAddr, uint64_t* pbegin, uint64_t* pspan, const char* tag)
{
    *pbegin = 0; *pspan = 0;
    if (!Readable((void*)vecAddr, 16)) return VEC_UNREADABLE;
    uint64_t b = 0, e = 0;
    memcpy(&b, (void*)vecAddr, 8);
    memcpy(&e, (void*)(vecAddr + 8), 8);
    if (e == b) return VEC_EMPTY;
    if (b < 0x10000 || e < b) return VEC_UNREADABLE;
    const uint64_t span = e - b;
    if (span > VEC_SANE_SPAN) {
        Log("[slice] %s: vector at %llx spans %llu bytes (begin=%llx end=%llx) -- past the "
            "%llu MB misread guard, a garbage pointer, treated as unreadable\n",
            tag, (unsigned long long)vecAddr, (unsigned long long)span,
            (unsigned long long)b, (unsigned long long)e,
            (unsigned long long)(VEC_SANE_SPAN >> 20));
        return VEC_UNREADABLE;
    }
    if (!Readable((void*)b, (size_t)span)) return VEC_UNREADABLE;
    *pbegin = b; *pspan = span;
    return VEC_OK;
}

// The span in bytes (0 when empty or unreadable), the shape ReadVec returns.
static uint64_t ReadVecAny(uint64_t vecAddr, uint64_t* pbegin, const char* tag)
{
    uint64_t b = 0, span = 0;
    if (ReadVecAnyEx(vecAddr, &b, &span, tag) != VEC_OK) return 0;
    *pbegin = b;
    return span;
}

// A vector of int32 (entity ids, load configs) into `out`, any length.
// false when unreadable or not a whole number of ints; an empty vector is true.
static bool ReadIntVec(uint64_t vecAddr, std::vector<int32_t>* out, const char* tag)
{
    out->clear();
    uint64_t b = 0, span = 0;
    const VecRead r = ReadVecAnyEx(vecAddr, &b, &span, tag);
    if (r == VEC_EMPTY) return true;
    if (r != VEC_OK) return false;
    if (span % 4) {
        Log("[slice] %s: int vector span %llu is not a multiple of 4 -- not a vector<int>\n",
            tag, (unsigned long long)span);
        return false;
    }
    out->resize((size_t)(span / 4));
    memcpy(out->data(), (const void*)b, (size_t)span);
    return true;
}


// ---------------------------------------------------------------------------
// Node decode. Established live and cross-validated: a2 == a3 + 0x70, the node
// vector's begin/end sit at a2+0x00, and each element is 24 bytes:
//     float x, y, z;  uint32 flags;  int32 type;  int32 id
// The id is a sequential negative placeholder (-1, -2, ...), and the depot's
// edge record referenced exactly those ids -- which is what makes this a decode
// rather than a plausible reading of a hexdump.
// ---------------------------------------------------------------------------
struct Node { float x, y, z; int32_t id; };

// Edge topology, which ROADN could not express.
//
// A road drawn against EXISTING infrastructure produces edges whose node0/node1
// are real positive entity ids, not placeholders -- capture #10 showed
// node0 = 281550 against node1 = -1. ROADN carried only node POSITIONS and
// rebuilt an all-new chain, so every connection to the existing world was
// silently dropped and the build was rejected on replay. Inferring "node i joins
// node i+1" is only correct for a road built in empty terrain.
// Tangents are carried, not synthesised.
//
// Deriving them from the chord (tangent = node1 - node0) makes every Hermite
// segment straight, so a curve replicates as a polygon of its control points --
// very visible on rail, which is drawn as long smooth arcs. The proposal already
// holds the real tangents at +0x10 and +0x1c; they were decoded early (the
// depot's (0,-20,0) matched its node delta exactly) and then simply never put on
// the wire.
// btype/bidx: BaseEdge::type (0 ground, 1 bridge, 2 tunnel) and typeIndex (the
// bridge/tunnel type resource index, -1 on the ground). The record is
// SegmentAndEntity { int entity; BaseEdge comp; int type; BaseEdgeStreet;
// BaseEdgeTrack; ... }: BaseEdge holds a std::vector (objects) so it is
// 8-aligned at +0x08 -- node0 +0x08, node1 +0x0c, tangents +0x10/+0x1c (the
// offsets already trusted below), then type +0x28, typeIndex +0x2c, the 24-byte
// objects vector +0x30..0x47, and the +0x48 street/track flag DecodeEdgeType
// reads. Without these two ints every replicated bridge came out as an
// embankment ("game infers landscape instead of a bridge", 2026-08-29).
struct Edge { int32_t node0, node1; float t0[3], t1[3]; int32_t btype, bidx; int32_t owner = -1; };

// A vector span past this is a misread pointer, not a command. The engine keeps
// a proposal's records in memory and nothing a player can do -- a road drag, a
// station with every module, a brush stroke -- comes anywhere near 1 GiB of
// them. It is not a content limit; tripping it is logged with the size so a
// refusal is never mistaken for one.
static const uint64_t PROPOSAL_SANITY_BYTES = 1ull << 30;

static void LogBadSpan(const char* what, uint64_t at, uint64_t span, uint64_t rec)
{
    Log("[slice] %s vector at %llx spans %llu B -- %s, not decoded\n", what,
        (unsigned long long)at, (unsigned long long)span,
        span % rec ? "not whole records" : "past the misread-pointer bound");
}

// Every edge record the vector holds, however many: a long road drag or a
// station upgrade re-adding all its internal track is one proposal, and the
// 0x20000-byte span (1,092 edges) plus the callers' fixed arrays this once had
// cut it short SILENTLY -- the tail never shipped. Returns the count, 0 for an
// empty vector, -1 (logged) when the pair is unreadable or not whole records.
static int DecodeEdgesVec(uint64_t a2, std::vector<Edge>* out, const char* tag)
{
    out->clear();
    uint64_t begin = 0, span = 0;
    const VecRead r = ReadVecAnyEx(a2 + 0x18, &begin, &span, tag);
    if (r == VEC_EMPTY) return 0;
    if (r != VEC_OK) return -1;
    if (span % 120 != 0) {
        Log("[slice] %s: edge vector span %llu is not whole 120-byte records\n", tag, (unsigned long long)span);
        return -1;
    }
    const int n = (int)(span / 120);
    out->resize((size_t)n);
    const uint8_t* b = (const uint8_t*)begin;
    for (int i = 0; i < n; i++) {
        Edge& e = (*out)[(size_t)i];
        memcpy(&e.node0, b + (size_t)i * 120 + 0x08, 4);
        memcpy(&e.node1, b + (size_t)i * 120 + 0x0c, 4);
        memcpy(e.t0,     b + (size_t)i * 120 + 0x10, 12);
        memcpy(e.t1,     b + (size_t)i * 120 + 0x1c, 12);
        memcpy(&e.btype, b + (size_t)i * 120 + 0x28, 4);
        memcpy(&e.bidx,  b + (size_t)i * 120 + 0x2c, 4);
        // Optional PlayerOwned: paired ownership-tool capture, 2026-09-18.
        // The replaced records have flag 0; otherwise identical new records
        // have flag 1 and the local player at +0x70. Never read an absent optional.
        const uint8_t owned = b[(size_t)i * 120 + 0x74];
        if (owned > 1) return -1;
        if (owned) {
            memcpy(&e.owner, b + (size_t)i * 120 + 0x70, 4);
            if (e.owner < 0) return -1;
        }
    }
    return n;
}

static int DecodeNodesVec(uint64_t a2, std::vector<Node>* out, const char* tag)
{
    out->clear();
    uint64_t begin = 0, span = 0;
    const VecRead r = ReadVecAnyEx(a2, &begin, &span, tag);
    if (r == VEC_EMPTY) return 0;
    if (r != VEC_OK) return -1;
    if (span % 24 != 0) {
        Log("[slice] %s: node vector span %llu is not whole 24-byte records\n", tag, (unsigned long long)span);
        return -1;
    }
    const int n = (int)(span / 24);
    out->resize((size_t)n);
    const uint8_t* b = (const uint8_t*)begin;
    for (int i = 0; i < n; i++) {
        Node& nd = (*out)[(size_t)i];
        memcpy(&nd.x,  b + (size_t)i * 24 + 0x00, 4);
        memcpy(&nd.y,  b + (size_t)i * 24 + 0x04, 4);
        memcpy(&nd.z,  b + (size_t)i * 24 + 0x08, 4);
        // The placeholder id (-1, -2, ...). Edges address nodes by THIS, not by
        // position in the vector, so it has to travel with the geometry.
        memcpy(&nd.id, b + (size_t)i * 24 + 0x14, 4);
    }
    return n;
}

// Vector-returning readers for the construction placement's ROADC companion
// (its caller keeps whole vectors too). Empty when the vector does not read or
// is empty; the -1 case is already logged by the *Vec reader.
static std::vector<Edge> DecodeEdges(uint64_t a2)
{
    std::vector<Edge> v;
    DecodeEdgesVec(a2, &v, "edges");
    return v;
}

static std::vector<Node> DecodeNodes(uint64_t a2)
{
    std::vector<Node> v;
    DecodeNodesVec(a2, &v, "nodes");
    return v;
}

// Edge type fields, decoded by diffing three builds: two roads of different
// types and one railway.
//
//   +0x48  edge type    0 = street, 1 = track
//   +0x4c  street type  25 / 22 for the two road types, -1 on a track
//   +0x04  track type   2 on the railway; a union slot holding unrelated bytes
//                       on streets, so it is only meaningful when type == 1
//
// Three samples separate these cleanly: a field that changes between the two
// ROADS cannot be the street/track flag, and a field that changes only on the
// RAILWAY cannot be the road type. One sample would have been guesswork -- the
// mistake that made -0.83147 look like a rotation matrix earlier today.
struct EdgeType { int type; int streetType; int trackType; bool catenary;
                  int hasBus; int tramTrackType; bool ok; };

static EdgeType DecodeEdgeType(uint64_t a2)
{
    EdgeType t = { 0, 16, 1, false, 0, 0, false };
    uint64_t begin = 0, end = 0;
    if (!Readable((void*)(a2 + 0x18), 16)) return t;
    memcpy(&begin, (void*)(a2 + 0x18), 8);
    memcpy(&end, (void*)(a2 + 0x20), 8);
    if (begin < 0x10000 || end <= begin) return t;
    uint64_t span = end - begin;
    if (span % 120 != 0 || !Readable((void*)begin, 120)) return t;
    const uint8_t* b = (const uint8_t*)begin;
    memcpy(&t.type, b + 0x48, 4);
    memcpy(&t.streetType, b + 0x4c, 4);
    memcpy(&t.trackType, b + 0x60, 4);
    // A street's bus lane and tram track sit immediately after streetType
    // (+0x4c) as two BYTES. Established differentially, not inferred: across
    // six upgrade captures the ONLY bytes that moved were +0x50 (00 -> 01
    // exactly when a bus lane was added) and +0x51 (00 -> 02 exactly when a
    // tram way was added); everything else in +0x48..0x6b was identical.
    // tramTrackType is the track TYPE, so it also carries electrification
    // (0 none, and the electrified tram shows as 2).
    // A street's bus lane is the byte at +0x50 and its TRAM TRACK TYPE is the
    // int at +0x54, both just past streetType (+0x4c).
    //
    // Established by controlled differential, after two wrong guesses. Holding
    // streetType constant at 25 and changing ONLY the tram selection, the sole
    // structural byte that moved was +0x54: 1 for a regular tram, 2 for an
    // electrified one (everything else that differed was node ids and tangent
    // floats, i.e. a different road segment). +0x51 was tried first and is NOT
    // a field: across fifteen captures it read 239 and 246, which is noise, not
    // a 0/1/2 enum -- shipping it stamped every tram electrified, which is why
    // a regular tram could not be built while electric-to-regular still worked.
    t.hasBus = b[0x50];
    memcpy(&t.tramTrackType, b + 0x54, 4);
    // Catenary is the low BYTE of +0x64; the upper three carry unrelated noise,
    // which is why reading the dword looked like chaos. Ground-truth sweep: every
    // catenary-on sample had low byte 01, every off sample 00, across 8 pairs.
    t.catenary = (b[0x64] & 1) != 0;
    if (t.type != 0 && t.type != 1) return t;          // not the layout we know
    // The type is an index into the loaded street/track type list, so only a
    // NEGATIVE one is impossible. There is no upper bound: the list is as long
    // as the installed mods make it, and the 512 this once refused (as "not the
    // layout") left a modded road type unreplicated -- the build ran natively
    // on one instance. The index travels as is and the peer applies it to its
    // own repository (roads.lua); the session's shared mod list is what keeps
    // the two lists equal, not a bound here.
    if (t.type == 0) {
        t.trackType = 1;                               // not applicable on a street
        if (t.streetType < 0) return t;
    } else {
        if (t.trackType < 0) return t;
    }
    t.ok = true;
    return t;
}

// ROADN carries every node, not just the endpoints. Collapsing a drawn road to
// first-and-last would replicate a straight line where the player drew a curve
// and still pass a hash check, because both peers would agree on the wrong road.
//
// Removed EDGES travel as full 8-token RECORDS (endpoints + tangents), the same
// shape ROADC already ships, not as entity ids: an id is meaningless on the peer
// (each instance numbers its own entities), while the two endpoint ids are
// positive existing nodes the Lua side can resolve to POSITIONS and look up
// again on the far end. Removed NODES stay ids and rn stays 0 -- no channel
// needs them yet.
static void WriteInject(const Node* nodes, int n, const Edge* edges, int m,
                        const int32_t* rmNode, int rn, const Edge* rmEdge, int re,
                        const EdgeType& et)
{
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return; }

    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return; }
    // ROADE <N> <etype> <stype> <ttype> <cat> <M> <rn> <re>
    //       <id x y z>*N
    //       <a1 a2 t0x t0y t0z t1x t1y t1z>*M
    //       <rmnodeid>*rn
    //       <a1 a2 t0x t0y t0z t1x t1y t1z>*re
    //       [<btype bidx>*M]
    // The Lua length check is  #w >= 9 + n*4 + m*8 + rn + re*8  (+ the optional
    // bridge tail). Both owners move together or the parser misreads the line.
    //
    // Node ids travel because edges reference them, and edge endpoints travel
    // verbatim because a positive value is a REAL entity in the existing world.
    // Sending a real id across peers is only sound if entity ids are identical
    // on both -- which the lockstep model already assumes but has never been
    // verified. If that assumption is wrong, this is where it will show up, as a
    // connecting road that lands on the wrong existing node rather than as a
    // silent failure.
    // The bus lane and tram track ride on their OWN line just ahead of the
    // ROADE. ROADE is positional and the Lua length-checks it, so widening it
    // would desynchronise both parsers; a tagged line consumed by the next
    // ROADE is the same shape ARMED already uses. Without this an upgrade that
    // ADDS a tram way or a bus lane had nothing to carry it, and since the
    // upgrade is cancelled and replayed from the wire the road came back plain
    // on every instance including the originator (2026-09-03).
    if (et.type == 0)
        fprintf(f, "STREETP %d %d\n", et.hasBus, et.tramTrackType);
    fprintf(f, "ROADE %d %d %d %d %d %d %d %d",
            n, et.type, et.streetType, et.trackType, et.catenary ? 1 : 0, m, rn, re);
    // Node z travels too. Re-deriving it from the terrain flattened every bridge
    // and embankment onto the ground -- the same mistake as the tangents, in a
    // different field: throwing away captured data and recomputing an
    // approximation of it.
    for (int i = 0; i < n; i++)
        fprintf(f, " %d %.4f %.4f %.4f", nodes[i].id, nodes[i].x, nodes[i].y, nodes[i].z);
    for (int i = 0; i < m; i++)
        fprintf(f, " %d %d %.4f %.4f %.4f %.4f %.4f %.4f",
                edges[i].node0, edges[i].node1,
                edges[i].t0[0], edges[i].t0[1], edges[i].t0[2],
                edges[i].t1[0], edges[i].t1[1], edges[i].t1[2]);
    for (int i = 0; i < rn; i++) fprintf(f, " %d", rmNode[i]);
    for (int i = 0; i < re; i++)
        fprintf(f, " %d %d %.4f %.4f %.4f %.4f %.4f %.4f",
                rmEdge[i].node0, rmEdge[i].node1,
                rmEdge[i].t0[0], rmEdge[i].t0[1], rmEdge[i].t0[2],
                rmEdge[i].t1[0], rmEdge[i].t1[1], rmEdge[i].t1[2]);
    // Bridge/tunnel TAIL, one <type idx> pair per added edge, APPENDED after the
    // whole legacy payload: the Lua length checks are ">=", so an old parser
    // ignores it and the new one reads it at the offset it computes itself.
    for (int i = 0; i < m; i++) fprintf(f, " %d %d", edges[i].btype, edges[i].bidx);
    // Local IPC only: inject.lua converts player entities to company numbers
    // before networking. Keep one entry per captured edge, including split halves.
    fprintf(f, " OWNERS");
    for (int i = 0; i < m; i++) fprintf(f, " %d", edges[i].owner);
    fprintf(f, "\n");
    fclose(f);
}

// EDEMO: a road/rail edge the player BULLDOZED.
//
// Until this existed the bulldozer was capture-only ("log only, never
// cancelled"): every other channel ADDS or edits, and the only removals that
// crossed the wire were constructions, vehicles, lines and stops. Demolishing a
// road was therefore a silent local-only edit -- the originator's road vanished
// and every peer kept theirs forever, which is an immediate e+z divergence. It
// was misread as a lag artifact because lag only widens the window in which the
// player does it; the demolish never replicated at any speed (2026-09-03).
//
// What travels is the removed edge's two ENDPOINT NODE IDS, not its entity id
// and not its geometry. The far end does not remove "edge 12345" -- it finds
// the node nearest each endpoint POSITION and takes the edge between them, so a
// divergent entity id cannot bulldoze the wrong road. The ids are resolved to
// positions by the Lua on THIS instance, which is why they can be ids here:
// they are only ever read locally.
//
// Endpoint nodes SURVIVE an edge-only demolish (rn == 0), so the Lua resolves
// them on its next tick even though the bulldoze has applied by then. When the
// bulldoze also removes nodes, those nodes are gone before the Lua looks -- so
// their positions are decoded HERE, while the proposal still describes them,
// and travel on the same line for the Lua to substitute.
// CDEMO <n> <id>...: a CONSTRUCTION demolish, shipped as the LOCAL entity ids
// the bulldozer was handed (r8+0x1e0 toRemove, docs/re/PROPOSALS.md). Ids do
// not travel; the Lua resolves each one to fileName + position ON THIS
// INSTANCE -- which it can, because the bulldoze was cancelled and the
// construction is still standing -- and ships that. Every instance, this one
// included, then bulldozes it at the stamp: the refund lands on the same
// sim-step everywhere (the coop money gap) and passengers are removed on the
// same step everywhere (the "strict demolish" ticket).
//
// Sanity before shipping, because a cancelled-but-undecodable demolish would
// silently destroy the player's action: a plausible count and positive ids.
// The Lua adds the real check (each id must carry a CONSTRUCTION component);
// if that fails nothing is replayed and the construction simply stays, which
// the player can see and redo.
// Every id the bulldozer was handed: a drag over a whole district is one
// command with as many constructions as it covered. The 16 this once refused
// (as "not a construction demolish") let the bulldoze run natively on the
// originator alone, with no notice to the peers.
static bool WriteCondemoInject(const std::vector<int32_t>& ids)
{
    const int nrem = (int)ids.size();
    if (nrem < 1) {
        Log("[slice] CDEMO: %d ids is not a construction demolish -- NOT shipped, not cancelled\n", nrem);
        return false;
    }
    for (int i = 0; i < nrem; i++) {
        if (ids[(size_t)i] <= 0) {
            Log("[slice] CDEMO: id[%d]=%d is not an entity -- NOT shipped, not cancelled\n", i, ids[(size_t)i]);
            return false;
        }
    }
    ReadInstance();
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return false; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return false; }
    fprintf(f, "CDEMO %d", nrem);
    for (int i = 0; i < nrem; i++) fprintf(f, " %d", ids[(size_t)i]);
    fprintf(f, "\n");
    fclose(f);
    Log("[slice] CDEMO shipped: %d construction(s), first id=%d\n", nrem, ids[0]);
    return true;
}

static bool WriteBulldozeInject(uint64_t nb, int rn, uint64_t eb, int re)
{
    if (re < 1) return false;
    ReadInstance();
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return false; }

    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return false; }
    // EDEMO <re> <rn> [<node0> <node1> <kind>]*re [<id> <x> <y> <z>]*rn
    //
    // The KIND (+0x48: 0 street, 1 track) is not decoration. A road node and a
    // rail node can sit at the SAME spot and are different nodes, and the far
    // end matches by position -- so without it a demolished rail endpoint can
    // snap onto the road node beside it and bulldoze the road instead. That is
    // exactly the geometry in play, since this was found demolishing roads at a
    // rail/road crossing. findNodeNear carries the same scar: it searched both
    // maps and welded track to street.
    fprintf(f, "EDEMO %d %d", re, rn);
    for (int i = 0; i < re; i++) {
        const uint8_t* b = (const uint8_t*)eb + (size_t)i * 120;
        int32_t n0 = 0, n1 = 0, kind = 0;
        memcpy(&n0, b + 0x08, 4);
        memcpy(&n1, b + 0x0c, 4);
        memcpy(&kind, b + 0x48, 4);
        fprintf(f, " %d %d %d", n0, n1, kind ? 1 : 0);
    }
    for (int i = 0; i < rn; i++) {
        const uint8_t* b = (const uint8_t*)nb + (size_t)i * 24;
        float x, y, z; int32_t nid = 0;
        memcpy(&x, b + 0x00, 4);
        memcpy(&y, b + 0x04, 4);
        memcpy(&z, b + 0x08, 4);
        memcpy(&nid, b + 0x14, 4);
        fprintf(f, " %d %.4f %.4f %.4f", nid, x, y, z);
    }
    fprintf(f, "\n");
    fclose(f);
    Log("[slice] EDEMO shipped: %d edge(s), %d removed node(s)\n", re, rn);
    return true;
}

// ROADC: the STREET part of a CONSTRUCTION placement proposal (caller 419f62).
// A depot/station snapped to a road integrates with the network inside the one
// placement command -- split of the snapped street plus connector edges. The
// construction itself replicates via CONP, but game.interface.buildConstruction
// on the peer cannot reproduce that integration, which is exactly the
// "replica overlaps the road but is not connected" report. Same record shapes
// as ROADE, except removed edges travel as FULL 8-token records (endpoints and
// tangents): the Lua side needs their geometry to classify each added edge as
// split-half (peer regenerates), frozen stub (CONP already builds it) or
// connector (the only part shipped onward). No rn field -- removed nodes are
// never needed for that classification.
//   ROADC <n> <etype> <stype> <ttype> <cat> <m> <re>
//         n x (id x y z)   m x (a1 a2 t0 t1)   re x (a1 a2 t0 t1)
//         m x (btype bidx)   ps=<placement serial>
// ps= is the same serial the placement's CONXP carries: the Lua pairs the two
// by it (cons.lua CM.flushConPairs), never by arrival order or distance.
static long g_conroad = 0;
static void WriteInjectConRoad(const Node* nodes, int n, const Edge* edges, int m,
                               const Edge* rme, int re, const EdgeType& et, long ps)
{
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return; }
    fprintf(f, "ROADC %d %d %d %d %d %d %d",
            n, et.type, et.streetType, et.trackType, et.catenary ? 1 : 0, m, re);
    for (int i = 0; i < n; i++)
        fprintf(f, " %d %.4f %.4f %.4f", nodes[i].id, nodes[i].x, nodes[i].y, nodes[i].z);
    for (int i = 0; i < m; i++)
        fprintf(f, " %d %d %.4f %.4f %.4f %.4f %.4f %.4f",
                edges[i].node0, edges[i].node1,
                edges[i].t0[0], edges[i].t0[1], edges[i].t0[2],
                edges[i].t1[0], edges[i].t1[1], edges[i].t1[2]);
    for (int i = 0; i < re; i++)
        fprintf(f, " %d %d %.4f %.4f %.4f %.4f %.4f %.4f",
                rme[i].node0, rme[i].node1,
                rme[i].t0[0], rme[i].t0[1], rme[i].t0[2],
                rme[i].t1[0], rme[i].t1[1], rme[i].t1[2]);
    // Bridge/tunnel tail (see WriteInject): <type idx> per ADDED edge.
    for (int i = 0; i < m; i++) fprintf(f, " %d %d", edges[i].btype, edges[i].bidx);
    fprintf(f, " ps=%ld\n", ps);
    fclose(f);
}

// ---------------------------------------------------------------------------
// DIAGNOSTIC DUMP HELPERS (dumpprop)
//
// Hex-dump a memory range in 64-byte chunks under one record tag. Chunked
// because Log() has a fixed buffer; the dump tools reassemble by offset.
static void GtDumpRange(const char* tag, int testId, int sample, const uint8_t* b,
                        unsigned len, unsigned baseOff)
{
    char line[400];
    for (unsigned off = 0; off < len; off += 64) {
        unsigned n = (len - off < 64) ? (len - off) : 64;
        int o = snprintf(line, sizeof(line), "[gt] %s%d.%d+%03x:", tag, testId, sample, baseOff + off);
        for (unsigned i = 0; i < n; i++)
            o += snprintf(line + o, sizeof(line) - o, "%02x", b[off + i]);
        Log("%s\n", line);
    }
}

// Read a std::vector<T>'s {begin,end} pair at vecAddr; returns the span in
// bytes (0 on any failure) and sets *pbegin. The whole span must be readable
// -- a partially readable vector is treated as no vector at all.
static uint64_t ReadVec(uint64_t vecAddr, uint64_t* pbegin, uint64_t maxSpan)
{
    if (!Readable((void*)vecAddr, 16)) return 0;
    uint64_t b = 0, e = 0;
    memcpy(&b, (void*)vecAddr, 8);
    memcpy(&e, (void*)(vecAddr + 8), 8);
    if (b < 0x10000 || e <= b) return 0;
    uint64_t span = e - b;
    if (span > maxSpan) return 0;
    if (!Readable((void*)b, (size_t)span)) return 0;
    *pbegin = b;
    return span;
}

// ReadVec against the misread bound, saying so when it trips: the size has to
// reach the log, or the refusal passes for a shape decision ("not a stroke").
static uint64_t ReadVecLoud(uint64_t vecAddr, uint64_t* pbegin, const char* what)
{
    const uint64_t span = ReadVec(vecAddr, pbegin, PROPOSAL_SANITY_BYTES);
    if (!span && Readable((void*)vecAddr, 16)) {
        uint64_t b = 0, e = 0;
        memcpy(&b, (void*)vecAddr, 8);
        memcpy(&e, (void*)(vecAddr + 8), 8);
        if (b >= 0x10000 && e > b && e - b > PROPOSAL_SANITY_BYTES)
            Log("[slice] %s vector at %llx spans %llu B -- past the misread-pointer bound, not read\n",
                what, (unsigned long long)vecAddr, (unsigned long long)(e - b));
    }
    return span;
}

