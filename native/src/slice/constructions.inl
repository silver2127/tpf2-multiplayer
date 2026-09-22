// constructions.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// construction params off the proposal (CONXP), stops, signals, module edits, upgrade shape, proposal dumps

// ---------------------------------------------------------------------------
// CONSTRUCTION PARAMS OFF THE PROPOSAL.
//
// The Lua captured a construction's params by reading e.params off the BUILT
// entity (lockstep.lua ~7130), which is why the originator had to let its native
// build stand (then bulldoze + rebuild at +0.6): cancel it and there was nothing
// to capture. That native-build-to-stamp window is the depot-placement vehicle
// drift -- host-only, the two replaying peers agree with each other (2026-09-08).
//
// The params are in the proposal all along. Decompiled (C:\tools\ghidra_out\
// decomp_ce: param_serialize_246, luatable_get/subtable/setkey, CE_ctor/copy_a,
// UI_UpdateConstruction_params): Proposal::ConstructionEntity is 0x8e0 B, the
// toAdd vector is at r8+0x1f8..+0x200 (the same vector MergeTemplateStreet and
// the bulldoze classifier read), and inside a ConstructionEntity:
//   +0x000 std::string fileName     +0x460 params lua::Table     +0x728 Mat4f transf
// lua::Table is an MSVC std::map<Variant,Variant>:
//   map  { _Myhead @0, _Mysize @8 }
//   node { _Left @0, _Parent @8, _Right @0x10, _Color @0x18, _Isnil @0x19, pair @0x20 }
//   key Variant @node+0x20 (tag @+0x40); value Variant @node+0x48 (tag @+0x68)
//   Variant = payload[0x20] + u8 tag: 2 = double @0, 3 = std::string @0 (SSO),
//   4 = nested map @0. Recursive, so a station's modules map is just a tag-4
//   value and one walker covers every construction.
// Emitted as the text lockstep.lua ser() makes (lockstep.lua:2684): [k]=v pairs,
// %.14g numbers (Lua tostring: "1" not "1.0"), %q strings, nested {} to any
// depth. In-order tree traversal is ser()'s own order (the map compares tag then
// value: numbers before strings, each ascending) -- and byte-equality is not
// load-bearing anyway: the peer only load()s the string (deserParams), and the
// edit tracker re-derives its baseline locally from the built entity (8220).
// Tags not yet observed (bool/nil) are logged RAW and omitted, exactly as ser()
// omits what it cannot serialise; the live dump names them.
// Nothing here is a content limit (2026-09-16). The old walker had a depth cap
// of 8 (a deeper table silently became {}), a 2048-node cap on the whole tree,
// a 4096-byte string cap and 256/1024-byte key/value buffers that CUT a longer
// string without a word. Now: strings of any length, tables of any size and
// depth, and every refusal is loud and refuses the WHOLE params -- the
// placement then runs natively with the notice, never with a partial literal.
// The two bounds that remain are misread-pointer guards: a string past 256 MiB
// or a map past 16M entries is not a params table but garbage.
static const uint64_t SSO_SANITY_LEN        = 256ull << 20;
static const uint64_t CONXP_SANITY_ENTRIES  = 1ull << 24;

// MSVC std::string (len @+0x10, cap @+0x18, chars inline iff cap < 16 else heap
// ptr @+0x00) -> out, whole. False on anything unreadable.
static bool ReadSsoString(uint64_t sa, std::string* out)
{
    out->clear();
    if (!Readable((void*)sa, 0x20)) return false;
    uint64_t len = 0, scap = 0;
    memcpy(&len, (void*)(sa + 0x10), 8);
    memcpy(&scap, (void*)(sa + 0x18), 8);
    if (scap < len) return false;
    if (len > SSO_SANITY_LEN) {
        Log("[sso] string at %llx claims len=%llu cap=%llu -- a misread pointer, refused\n",
            (unsigned long long)sa, (unsigned long long)len, (unsigned long long)scap);
        return false;
    }
    const char* chars = nullptr;
    if (scap < 16) chars = (const char*)sa;
    else {
        uint64_t hp = 0;
        memcpy(&hp, (void*)sa, 8);
        if (IsHeapPtr(hp) && Readable((void*)hp, (size_t)len)) chars = (const char*)hp;
    }
    if (!chars) return false;
    out->assign(chars, (size_t)len);
    return true;
}
// The fixed-buffer form for a reader that keeps a char array (the stop name):
// cut to cap-1 there, which is that reader's own bound, not the reader's.
static bool ReadSsoString(uint64_t sa, char* out, size_t cap)
{
    out[0] = 0;
    std::string s;
    if (!ReadSsoString(sa, &s)) return false;
    const size_t take = s.size() < cap - 1 ? s.size() : cap - 1;
    memcpy(out, s.data(), take);
    out[take] = 0;
    return true;
}

struct ConxpOut { std::string s; };
static void CoPut(ConxpOut* o, const char* t) { o->s.append(t); }
// Lua %q: double-quoted, " \ and control characters escaped so load() takes it
// back. Length-aware: an embedded NUL is escaped like any other control byte.
static void CoPutQ(ConxpOut* o, const std::string& t)
{
    o->s.push_back('"');
    char tmp[8];
    for (size_t i = 0; i < t.size(); i++) {
        const unsigned char c = (unsigned char)t[i];
        if (c == '"' || c == '\\') { o->s.push_back('\\'); o->s.push_back((char)c); }
        else if (c == '\n') o->s.append("\\n");
        else if (c == '\r') o->s.append("\\r");
        else if (c < 32 || c == 127) { snprintf(tmp, sizeof(tmp), "\\%03u", (unsigned)c); o->s.append(tmp); }
        else o->s.push_back((char)c);
    }
    o->s.push_back('"');
}
static void CoPutNum(ConxpOut* o, double d)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.14g", d);
    o->s.append(tmp);
}

// Walk state: the entry count, and the maps on the current descent -- the cycle
// guard that took the depth cap's place (see SerLuaTable).
struct ConxpWalk { int nodes; std::vector<uint64_t> open; };

// 1 = emitted, 0 = omitted (a tag ser() cannot serialise either, logged),
// -1 = the params are unusable (unreadable, cyclic, absurd): refuse them all.
static int SerLuaValue(ConxpOut* o, uint64_t var, int depth, ConxpWalk* w);

// One lua::Table (an MSVC _Tree), walked in order and emitted as a Lua literal.
// False = refuse the whole params; the output is rolled back to where it was.
static bool SerLuaTable(ConxpOut* o, uint64_t map, int depth, ConxpWalk* w)
{
    // A lua::Table owns its nested tables by value, so no map can contain an
    // ancestor: meeting one again on the way down means the walk is reading
    // garbage. Engine tables never trip this; it replaces the old depth cap.
    for (uint64_t a : w->open)
        if (a == map) {
            Log("[conxp] table %llx met again at depth %d -- a cycle, refusing the params\n",
                (unsigned long long)map, depth);
            return false;
        }
    if (!Readable((void*)map, 0x10)) return false;
    uint64_t head = 0, size = 0;
    memcpy(&head, (void*)map, 8);
    memcpy(&size, (void*)(map + 8), 8);
    if (!IsHeapPtr(head) || !Readable((void*)head, 0x70)) return false;
    if (size > CONXP_SANITY_ENTRIES) {
        Log("[conxp] table %llx claims %llu entries at depth %d -- a misread pointer, refusing the params\n",
            (unsigned long long)map, (unsigned long long)size, depth);
        return false;
    }
    w->open.push_back(map);
    const size_t mark0 = o->s.size();
    auto refuse = [&](const char* why, uint64_t seen) {
        Log("[conxp] table %llx: %s at entry %llu of %llu (depth %d) -- refusing the params\n",
            (unsigned long long)map, why, (unsigned long long)seen, (unsigned long long)size, depth);
        o->s.resize(mark0);
        w->open.pop_back();
        return false;
    };
    CoPut(o, "{");
    bool first = true;
    uint64_t seen = 0;
    uint64_t node = 0;
    memcpy(&node, (void*)head, 8);                      // _Myhead->_Left = begin()
    while (node && node != head) {
        // The tree holds exactly `size` nodes: walking past that is a corrupt
        // tree, and an unreadable node used to END the walk with a partial
        // literal on the wire. Both refuse.
        if (++seen > size) return refuse("walked past its own size", seen);
        if (!Readable((void*)node, 0x70)) return refuse("unreadable node", seen);
        w->nodes++;
        uint8_t ktag = *(const uint8_t*)(node + 0x40);
        size_t mark = o->s.size();
        bool ok = false;
        if (!first) CoPut(o, ",");
        CoPut(o, "[");
        if (ktag == 2) { double k = 0; memcpy(&k, (void*)(node + 0x20), 8); CoPutNum(o, k); ok = true; }
        else if (ktag == 3) {
            std::string ks;
            if (!ReadSsoString(node + 0x20, &ks)) return refuse("unreadable string key", seen);
            CoPutQ(o, ks); ok = true;
        }
        else Log("[conxp]   key tag %u unknown (depth %d) -- entry skipped\n", (unsigned)ktag, depth);
        if (ok) {
            CoPut(o, "]=");
            const int r = SerLuaValue(o, node + 0x48, depth + 1, w);
            if (r < 0) return refuse("unusable value", seen);
            ok = r == 1;
        }
        if (ok) first = false; else o->s.resize(mark);
        // in-order successor (MSVC _Tree): leftmost of the right subtree, else
        // climb while we are our parent's right child; the sentinel ends it.
        uint64_t nx = 0;
        memcpy(&nx, (void*)(node + 0x10), 8);
        if (nx && Readable((void*)nx, 0x1a) && !*(const uint8_t*)(nx + 0x19)) {
            node = nx;
            for (;;) {
                uint64_t l = 0;
                memcpy(&l, (void*)node, 8);
                if (!l || !Readable((void*)l, 0x1a) || *(const uint8_t*)(l + 0x19)) break;
                node = l;
            }
        } else {
            uint64_t cur = node;
            for (;;) {
                uint64_t par = 0;
                if (!Readable((void*)cur, 0x1a)) { cur = head; break; }
                memcpy(&par, (void*)(cur + 8), 8);
                if (!par || !Readable((void*)par, 0x1a)) { cur = head; break; }
                uint64_t pr = 0;
                memcpy(&pr, (void*)(par + 0x10), 8);
                if (pr != cur) { cur = par; break; }
                cur = par;
                if (cur == head) break;
            }
            node = cur;
        }
    }
    if (seen != size) return refuse("ended short of its own size", seen);
    CoPut(o, "}");
    w->open.pop_back();
    return true;
}

static int SerLuaValue(ConxpOut* o, uint64_t var, int depth, ConxpWalk* w)
{
    if (!Readable((void*)var, 0x28)) return -1;
    uint8_t tag = *(const uint8_t*)(var + 0x20);
    // tag 1 = boolean, value in payload byte 0 (Lua type order: nil, boolean,
    // number, string, table). A modular station carries ~20 of these in its
    // modules metadata; omitting them made the rebuilt proposal an
    // "internal error" (2026-09-08, the first modular station placed with the
    // construction cancel).
    if (tag == 1) { uint8_t b = 0; memcpy(&b, (void*)var, 1); CoPut(o, b ? "true" : "false"); return 1; }
    if (tag == 2) { double d = 0; memcpy(&d, (void*)var, 8); CoPutNum(o, d); return 1; }
    if (tag == 3) { std::string t; if (!ReadSsoString(var, &t)) return -1; CoPutQ(o, t); return 1; }
    if (tag == 4) return SerLuaTable(o, var, depth, w) ? 1 : -1;
    uint64_t q0 = 0;
    memcpy(&q0, (void*)var, 8);
    Log("[conxp]   value tag %u unknown (depth %d) payload0=%016llx -- omitted\n",
        (unsigned)tag, depth, (unsigned long long)q0);
    return 0;
}

// Serialise the FIRST toAdd ConstructionEntity of the factory's Proposal (r8)
// into the stash. False = stash empty = do NOT cancel (the build runs natively
// and today's capture path takes over): never cancel on data we cannot replay.
static bool StashConxpFromProposal(uint64_t r8)
{
    g_conxpFile.clear(); g_conxpParams.clear();
    if (!Readable((void*)(r8 + 0x1f8), 16)) return false;
    uint64_t cb = 0, ce = 0;
    memcpy(&cb, (void*)(r8 + 0x1f8), 8);
    memcpy(&ce, (void*)(r8 + 0x200), 8);
    if (!IsHeapPtr(cb) || ce < cb + 0x8e0 || !Readable((void*)cb, 0x8e0)) return false;
    if (!ReadSsoString(cb, &g_conxpFile) || g_conxpFile.empty()) return false;
    memcpy(g_conxpT, (void*)(cb + 0x728), sizeof(g_conxpT));
    ConxpOut o;
    ConxpWalk w = { 0, {} };
    bool ok = SerLuaTable(&o, cb + 0x460, 0, &w);
    if (!ok || w.nodes == 0) {
        Log("[conxp] params walk %s (nodes=%d) -- not shipped\n",
            !ok ? "failed" : "found no entries", w.nodes);
        g_conxpParams.clear();
        return false;
    }
    g_conxpParams.swap(o.s);
    const int nodes = w.nodes;
    Log("[conxp] %s pos=(%.1f,%.1f,%.1f) params(%d node(s), %zu B)=%.600s%s\n", g_conxpFile.c_str(),
        g_conxpT[12], g_conxpT[13], g_conxpT[14], nodes, g_conxpParams.size(), g_conxpParams.c_str(),
        g_conxpParams.size() > 600 ? "..." : "");
    // PROBE (2026-09-08): does a construction placement carry the footprint
    // buildings the engine is about to demolish, in the proposal's toRemove
    // vector<int> at r8+0x1e0? If it does, the cancel flow can ship that exact
    // set (resolved to positions on the originator, whose entities still exist
    // because the build was cancelled before the demolish ran) instead of
    // guessing a footprint box. Log the count + first ids so it can be verified
    // against a placement made over known buildings. Read-only, guarded.
    if (Readable((void*)(r8 + 0x1e0), 16)) {
        uint64_t rb = 0, re2 = 0;
        memcpy(&rb, (void*)(r8 + 0x1e0), 8);
        memcpy(&re2, (void*)(r8 + 0x1e8), 8);
        if (IsHeapPtr(rb) && re2 >= rb) {
            int cnt = (int)((re2 - rb) / 4);
            char ids[256]; int o2 = 0; ids[0] = 0;
            if (Readable((void*)rb, (size_t)(cnt < 64 ? cnt : 64) * 4))
                for (int i = 0; i < cnt && i < 12 && o2 < (int)sizeof(ids) - 12; i++)
                    o2 += snprintf(ids + o2, sizeof(ids) - o2, "%s%d", i ? "," : "", *(const int32_t*)(rb + i * 4));
            Log("[conxp]   toRemove(r8+0x1e0) count=%d ids=[%s]%s\n", cnt, ids,
                cnt == 0 ? " -- EMPTY: footprint demolish is NOT in the make-time proposal" : "");
        } else {
            Log("[conxp]   toRemove(r8+0x1e0) not a vector (rb=%llx re=%llx)\n",
                (unsigned long long)rb, (unsigned long long)re2);
        }
    }
    return true;
}

// CONXP <file> t=<16 floats> ps=<serial> rc=<0|1> params=<lua literal>: the
// construction half of a CANCELLED placement, for the Lua to seat in pendingCons
// where the entity poll would have (there is no entity). Written from the Add
// hook, cancel confirmed. ps= is the placement serial its ROADC carries, rc=
// whether one was written for it (0: free-standing, no payload to wait for).
static void WriteInjectConxp()
{
    ReadInstance();
    if (!g_instance[0] || g_conxpFile.empty()) return;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return; }
    fprintf(f, "CONXP %s t=", g_conxpFile.c_str());
    for (int i = 0; i < 16; i++) fprintf(f, "%s%.4f", i ? "," : "", g_conxpT[i]);
    fprintf(f, " ps=%ld rc=%d params=", g_conxpSerial, g_conxpHadRoadc);
    fwrite(g_conxpParams.data(), 1, g_conxpParams.size(), f);
    fputc('\n', f);
    fclose(f);
    Log("[slice] CONXP shipped: %s ps=%ld rc=%d (%zu B params)\n", g_conxpFile.c_str(),
        g_conxpSerial, g_conxpHadRoadc, g_conxpParams.size());
    g_conxpFile.clear();
}

// ---------------------------------------------------------------------------
// STOP / SIGNAL / WAYPOINT OFF THE PROPOSAL.
//
// The tool's proposal is: removedSegments (r8+0x48) = the edge being rebuilt
// (one 120-B SegmentAndEntity, entity @+0x00 -- the REAL edge id, still valid
// on the originator because the build is cancelled) and edgeObjectsToAdd
// (r8+0xf8) = one 0x100-B record, built by 0x21ef7d0 (decompiled 2026-09-08)
// and cross-checked against six live records (four stops, two signals, one
// waypoint) and what the entity poll read back off the BUILT objects:
//   +0x00 edgeEntity (-1 = the rebuilt edge)
//   +0x04 0 for a street stop, 2 for a track object (signal/waypoint)
//   +0x08 -1 (an entity slot, unused for a fresh placement)
//   +0x10 modelId                       +0x14 Mat4f transf (x,y,z @+0x44/48/4c)
//   +0xd0 the commit's bool argument    +0xd1 ENGINE `left` (u8)
//   +0xd8 std::string name (SSO)        +0xf8 playerEntity
// +0xd1 is the byte the engine's STOP_LEFT/STOP_RIGHT comes from (poll side=0
// <=> +0xd1=1, three stops). For a TRACK object it is NOT the geometric side
// of the model -- two signals that both stood geometrically left of their
// edge carried 0 and 1 -- which is exactly why the poll path, reading the
// side off geometry through the street convention, built signals facing the
// wrong way. Ship the engine's own byte. +0xd0 is provisionally `oneWay`
// (the only bool the commit passes down; 0 on every sample, none one-way):
// the [stop] line logs it so a one-way placement pins or refutes it.
// A placement that REPLACES an object (edgeObjectsToRemove non-empty) is not
// cancelled: the engine re-points that stop's lines (old2newEdgeObjects),
// which a script proposal cannot carry -- it builds natively, and the stop tool's
// NATIVE notice gets it to the mod's catch-up scan and its STOPREP path.
static bool StashStopFromProposal(uint64_t r8)
{
    g_stopName.clear(); g_stopEid = -1;
    uint64_t rb = 0;
    if (ReadVecAny(r8 + 0x48, &rb, "STOP removedSegments") < 120 || !Readable((void*)rb, 120)) return false;
    int32_t eid = -1;
    memcpy(&eid, (void*)rb, 4);
    if (eid < 0) return false;
    // edgeObjectsToRemove at any length: the 0x4000-byte span this once read
    // through would have read a LONGER list as empty and cancelled a
    // replacement as a plain placement.
    uint64_t xb = 0;
    if (ReadVecAny(r8 + 0xe0, &xb, "STOP edgeObjectsToRemove") >= 0x100) {
        Log("[stop] placement replaces an object -- not cancelled, the catch-up scan's STOPREP path handles it\n");
        return false;
    }
    uint64_t ob = 0;
    if (ReadVecAny(r8 + 0xf8, &ob, "STOP edgeObjectsToAdd") < 0x100 || !Readable((void*)ob, 0x100)) return false;
    int32_t kind = -1, model = 0, player = 0;
    memcpy(&kind, (void*)(ob + 0x04), 4);
    memcpy(&model, (void*)(ob + 0x10), 4);
    memcpy(&player, (void*)(ob + 0xf8), 4);
    if ((kind < 0 || kind > 2) || model <= 0) return false;
    float pos[3];
    memcpy(pos, (void*)(ob + 0x44), 12);
    uint8_t b0 = *(const uint8_t*)(ob + 0xd0), left = *(const uint8_t*)(ob + 0xd1);
    if (!ReadStdString(ob + 0xd8, &g_stopName, "STOP name")) g_stopName.clear();
    g_stopEid = eid; g_stopSide = kind; g_stopModel = model; g_stopPlayer = player;
    memcpy(g_stopPos, pos, 12); g_stopLeft = left ? 1 : 0; g_stopOneWay = b0 ? 1 : 0;
    Log("[stop] edge=%d kind=%d model=%d pos=(%.1f,%.1f,%.1f) left=%u b0(oneWay?)=%u player=%d name='%s' diag +08=%08x +d0..d3=%02x%02x%02x%02x\n",
        eid, kind, model, pos[0], pos[1], pos[2], (unsigned)left, (unsigned)b0, player, g_stopName.c_str(),
        *(const uint32_t*)(ob + 0x08), (unsigned)b0, (unsigned)left,
        (unsigned)*(const uint8_t*)(ob + 0xd2), (unsigned)*(const uint8_t*)(ob + 0xd3));
    return true;
}

// STOPX <edge> <kind> <modelId> <x> <y> <z> <left> <oneWay> <player> name=<rest>
// ARMED 1 precedes it: the Lua ships the STOPADD without skipOrigin, so the
// originator replays it through the very path the peers use (strict).
static void WriteInjectStop()
{
    ReadInstance();
    if (!g_instance[0] || g_stopEid < 0) return;
    WriteArmed(true);
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return; }
    fprintf(f, "STOPX %d %d %d %.4f %.4f %.4f %u %u %d name=%s\n",
            g_stopEid, g_stopSide, g_stopModel, g_stopPos[0], g_stopPos[1], g_stopPos[2],
            (unsigned)g_stopLeft, (unsigned)g_stopOneWay, g_stopPlayer, g_stopName.c_str());
    fclose(f);
    Log("[slice] STOPX shipped: edge=%d kind=%d model=%d left=%u\n", g_stopEid, g_stopSide, g_stopModel, (unsigned)g_stopLeft);
    g_stopEid = -1;
}

// ---------------------------------------------------------------------------
// STOP / SIGNAL BULLDOZE OFF THE PROPOSAL.
//
// The bulldozer removes an edge object by RE-ADDING its edge without it:
// removedSegments[0] is the edge as it stands and addedSegments[0] the same
// edge with the survivors only. Each 120-B SegmentAndEntity carries its
// `objects` as a std::vector of 8-B {entity, type} pairs at +0x30 (read off
// the live rmSeg hex of a truck-stop bulldoze, 2026-09-08: one pair, the
// stop's entity). The removed object is the set difference -- exactly one
// for a bulldoze, or this is not cancelled.
//
// Why strict: the host used to remove the stop natively at click time while
// the peers rebuilt the edge two sim-steps later through a script proposal.
// Passengers already walking to that stop re-planned on different steps,
// the people count diverged ~50 units later, and the buses drifted from
// the different dwell times (session 2026-09-08 t=635 -> 696 -> 2456).
// The edge's whole object list, however long: a busy street edge carries
// every stop, signal and waypoint on it, and the 128 records / first-16 cut
// this once made called an object past the sixteenth "not ours" -- its
// removal then ran natively on the originator alone. -1 when unreadable.
static int ReadObjList(uint64_t seg, std::vector<int32_t>* out)
{
    out->clear();
    uint64_t b = 0, span = 0;
    const VecRead r = ReadVecAnyEx(seg + 0x30, &b, &span, "STOPXDEL edge objects");
    if (r == VEC_EMPTY) return 0;                           // an edge with no objects
    if (r != VEC_OK || span % 8) return -1;
    const int n = (int)(span / 8);
    out->resize((size_t)n);
    for (int i = 0; i < n; i++) memcpy(&(*out)[(size_t)i], (void*)(b + (uint64_t)i * 8), 4);
    return n;
}

static bool StashStopDelFromBulldoze(uint64_t eb, int re, uint64_t adb, int aedges)
{
    g_stopDelEo = -1; g_stopDelEdge = -1;
    if (re != 1 || aedges != 1) {
        Log("[stop] bulldoze re=%d addEdges=%d -- not a single-edge object removal, not cancelled\n", re, aedges);
        return false;
    }
    std::vector<int32_t> before, after;
    int nb = ReadObjList(eb, &before), na = ReadObjList(adb, &after);
    if (nb < 0 || na < 0) {
        Log("[stop] bulldoze: edge object list unreadable (rm=%d add=%d) -- not cancelled\n", nb, na);
        return false;
    }
    int32_t gone = -1; int ngone = 0;
    for (int i = 0; i < nb; i++) {
        bool kept = false;
        for (int j = 0; j < na; j++) if (after[(size_t)j] == before[(size_t)i]) kept = true;
        if (!kept) { gone = before[(size_t)i]; ngone++; }
    }
    int32_t edge = -1; memcpy(&edge, (void*)eb, 4);
    if (ngone != 1 || gone <= 0) {
        Log("[stop] bulldoze on edge %d: %d object(s) before, %d after, %d gone -- not exactly one, not cancelled\n", edge, nb, na, ngone);
        return false;
    }
    g_stopDelEo = gone; g_stopDelEdge = edge;
    Log("[stop] bulldoze removes edge object %d from edge %d (%d -> %d object(s))\n", gone, edge, nb, na);
    return true;
}

// STOPXDEL <edgeObject> <edge>. ARMED 1 precedes it: the Lua ships a STOPDEL
// without skipOrigin, so the originator removes it at the stamp like a peer.
static void WriteInjectStopDel()
{
    ReadInstance();
    if (!g_instance[0] || g_stopDelEo < 0) return;
    WriteArmed(true);
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return; }
    fprintf(f, "STOPXDEL %d %d\n", g_stopDelEo, g_stopDelEdge);
    fclose(f);
    Log("[slice] STOPXDEL shipped: object=%d edge=%d\n", g_stopDelEo, g_stopDelEdge);
    g_stopDelEo = -1;
}

// A construction UPGRADE proposal: toRemove[0] is the entity being replaced,
// toAdd[0] the ConstructionEntity that replaces it (same file, new params --
// a module added or removed, a station upgraded). Reuses the placement stash
// for the new CE; only the old id is extra.
static bool StashConupFromProposal(uint64_t r8)
{
    g_conupOldId = 0;
    if (!Readable((void*)(r8 + 0x1e0), 16)) return false;
    uint64_t rb = 0, re = 0;
    memcpy(&rb, (void*)(r8 + 0x1e0), 8);
    memcpy(&re, (void*)(r8 + 0x1e8), 8);
    if (!IsHeapPtr(rb) || re < rb + 4 || !Readable((void*)rb, 4)) return false;
    int32_t old = 0;
    memcpy(&old, (void*)rb, 4);
    if (old <= 0) return false;
    if (!StashConxpFromProposal(r8)) return false;
    g_conupOldId = old;
    return true;
}

// CONUP <oldEntity> <file> t=<16 floats> params=<lua literal>: a CANCELLED
// construction upgrade. The Lua resolves the old entity to its position (it
// still stands -- the upgrade was cancelled) and every instance, this one
// included, upgradeConstruction()s it to these params at the stamp.
static void WriteInjectConup()
{
    ReadInstance();
    if (!g_instance[0] || g_conxpFile.empty() || g_conupOldId <= 0) return;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return; }
    fprintf(f, "CONUP %d %s t=", g_conupOldId, g_conxpFile.c_str());
    for (int i = 0; i < 16; i++) fprintf(f, "%s%.4f", i ? "," : "", g_conxpT[i]);
    fputs(" params=", f);
    fwrite(g_conxpParams.data(), 1, g_conxpParams.size(), f);
    fputc('\n', f);
    fclose(f);
    Log("[slice] CONUP shipped: old=%d %s (%zu B params)\n", g_conupOldId, g_conxpFile.c_str(), g_conxpParams.size());
    g_conxpFile.clear(); g_conupOldId = 0;
}

// Shape test for an upgrade proposal: something removed, a CE added, no new
// street nodes (a placement adds nodes; an upgrade never does).
static bool IsUpgradeShape(uint64_t r8)
{
    __try {
        uint64_t b = 0;
        int nadd = 0, nrem = 0;
        uint64_t tspan = ReadVecAny(r8 + 0x1e0, &b, "upgrade-shape toRemove");
        nrem = (int)(tspan / 4);
        if (Readable((void*)(r8 + 0x1f8), 16)) {
            uint64_t ab = 0, ae = 0;
            memcpy(&ab, (void*)(r8 + 0x1f8), 8);
            memcpy(&ae, (void*)(r8 + 0x200), 8);
            if (ae > ab) nadd = (int)((ae - ab) / 0x8e0);
        }
        // NOT gated on "no new nodes": a modular-station upgrade re-adds every
        // internal track node (measured 2026-09-08, caller 42bc7b: addNodes=32
        // rmNodes=25 for a platform added). toRemove+toAdd is the discriminator;
        // placements (which have an EMPTY toRemove) never reach this branch anyway.
        if (nrem >= 1 && nadd >= 1)
            Log("[slice] upgrade shape: toRemove=%d toAdd=%d\n", nrem, nadd);
        return nrem >= 1 && nadd >= 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// DUMPPROP: differential dump of a construction placement proposal.
//
// The sol2 SimpleProposal path and the UI's ConstructionBuilder hand the same
// factory the same struct type; what differs is CONTENT -- the linkage between
// the construction entity and the street placeholders that the API cannot
// express. Dumping both, same instance, same code path, and diffing offline is
// the ground-truth way to find where that linkage lives.
//   [gt] D8<c>.0+off  hex of r8[0..0x240)        (c: 1 = UI, 2 = Lua)
//   [gt] D9<c>.0+off  hex of r9[0..0x480)
//   [slice] DVEC r8+off span=N elem?=...           every plausible vector in r8
//   [gt] DV<c>.<k>+off hex of that vector's first bytes
// plus ChaseStrings over both roots (fileName, module tags, names).
static void DumpVectors(int c, uint64_t base, unsigned len)
{
    int k = 0;
    for (unsigned off = 0; off + 24 <= len; off += 8) {
        if (!Readable((void*)(base + off), 24)) continue;
        uint64_t b = 0, e = 0, cap = 0;
        memcpy(&b, (void*)(base + off), 8);
        memcpy(&e, (void*)(base + off + 8), 8);
        memcpy(&cap, (void*)(base + off + 16), 8);
        if (!IsHeapPtr(b) || e < b || cap < e) continue;
        uint64_t span = e - b;
        if (span == 0 || span > 0x4000) continue;
        if (!Readable((void*)b, (size_t)(span > 0x200 ? 0x200 : span))) continue;
        Log("[slice] DVEC c=%d r8+%03x span=%llu (/4=%llu /24=%llu /120=%llu)\n",
            c, off, (unsigned long long)span, (unsigned long long)(span / 4),
            (unsigned long long)(span / 24), (unsigned long long)(span / 120));
        char tag[16];
        snprintf(tag, sizeof(tag), "DV%d_%03x_", c, off);
        GtDumpRange(tag, 0, k++, (const uint8_t*)b, (unsigned)(span > 0x200 ? 0x200 : span), 0);
    }
}

static void DumpProposal(int c, uint64_t r8, uint64_t r9)
{
    __try {
        Log("[slice] DUMPPROP c=%d (%s) r8=%llx r9=%llx\n", c, c == 1 ? "UI" : "Lua",
            (unsigned long long)r8, (unsigned long long)r9);
        if (Readable((void*)r8, 0x240)) {
            GtDumpRange(c == 1 ? "D8u_" : (c == 2 ? "D8l_" : "D8m_"), 0, 0, (const uint8_t*)r8, 0x240, 0);
            DumpVectors(c, r8, 0x240);
            ChaseStrings(0, c, r8, 0x240);
        }
        if (Readable((void*)r9, 0x480)) {
            GtDumpRange(c == 1 ? "D9u_" : (c == 2 ? "D9l_" : "D9m_"), 0, 0, (const uint8_t*)r9, 0x480, 0);
            ChaseStrings(0, c + 10, r9, 0x480);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[slice] DUMPPROP fault\n");
    }
}
