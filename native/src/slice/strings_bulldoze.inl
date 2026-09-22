// strings_bulldoze.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// heap pointer and std::string readers, percent encoding, the bulldozer's classification

// ---------------------------------------------------------------------------
// Pointer chase for strings. The params.modules map of a construction is a
// native map<int, ModuleInfo> that is NOT in the raw proposal bytes -- the M8
// probe reached 'platform_cargo_era_a.module' at ProposalData +0xe8+0x40+0x38,
// three hops down. So: breadth-first over every qword that looks like a heap
// pointer, up to three levels, reporting any printable run that contains one
// of the needles together with the offset path that reached it. The path IS
// the layout.
static bool IsHeapPtr(uint64_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFFULL; }

// An MSVC std::string at sa, at ANY length: 16-byte SSO buffer, size at +0x10,
// capacity at +0x18; the text sits inline while the capacity is 15 or less and
// past that +0x00 points at it. A name is whatever the player typed -- the 255
// characters the line and vehicle renames once stopped at refused the rename
// ("unreadable or empty -- not shipped") and it applied on one instance only.
// False when the struct or the text does not read; the only bound is the
// misread guard on the length.
static bool ReadStdString(uint64_t sa, std::string* out, const char* tag)
{
    out->clear();
    if (!Readable((void*)sa, 32)) return false;
    uint64_t len = 0, cap = 0;
    memcpy(&len, (void*)(sa + 0x10), 8);
    memcpy(&cap, (void*)(sa + 0x18), 8);
    if (len > cap) return false;
    if (len > VEC_SANE_SPAN) {
        Log("[slice] %s: string at %llx claims %llu bytes -- past the misread guard, not a string\n",
            tag, (unsigned long long)sa, (unsigned long long)len);
        return false;
    }
    const char* src = (const char*)sa;
    if (cap > 15) {
        uint64_t ptr = 0; memcpy(&ptr, (void*)sa, 8);
        if (!IsHeapPtr(ptr)) return false;
        src = (const char*)ptr;
    }
    if (!Readable((void*)src, (size_t)len + 1)) return false;
    out->assign(src, (size_t)len);
    return true;
}

// Percent-encoded for the inject file, which the Lua splits on whitespace:
// printable ASCII other than '%' and '=' travels as is, everything else
// (spaces, UTF-8, the two escape characters) as %XX. CM.unescName undoes it.
static std::string PercentEncode(const std::string& s)
{
    std::string enc;
    enc.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch > 32 && ch < 127 && ch != '%' && ch != '=') enc.push_back((char)ch);
        else { char h[4]; snprintf(h, sizeof(h), "%%%02X", ch); enc.append(h, 3); }
    }
    return enc;
}

static void ChaseStrings(int testId, int sample, uint64_t root, unsigned rootLen)
{
    struct Item { uint64_t base; unsigned len; int depth; char path[64]; };
    static Item queue[900];
    int head = 0, tail = 0, visited = 0, hits = 0;
    Item r; r.base = root; r.len = rootLen; r.depth = 0; snprintf(r.path, sizeof(r.path), "a3");
    queue[tail++] = r;
    while (head < tail && visited < 800) {
        Item it = queue[head++];
        visited++;
        if (!Readable((void*)it.base, it.len)) continue;
        const uint8_t* b = (const uint8_t*)it.base;
        // 1) inline printable runs containing a needle
        for (unsigned i = 0; i + 8 <= it.len; i++) {
            if (b[i] < 32 || b[i] > 126) continue;
            unsigned j = i; while (j < it.len && b[j] >= 32 && b[j] <= 126) j++;
            unsigned n = j - i;
            if (n >= 8) {
                char tmp[160]; unsigned take = n < 159 ? n : 159;
                memcpy(tmp, b + i, take); tmp[take] = 0;
                if (strstr(tmp, ".module") || strstr(tmp, "station/") ||
                    strstr(tmp, ".con") || strstr(tmp, ".lua")) {
                    Log("[gt] S%d.%d %s+%03x \"%s\"\n", testId, sample, it.path, i, tmp);
                    if (++hits > 120) return;
                }
            }
            i = j;
        }
        // 2) follow pointers one level deeper
        if (it.depth >= 3) continue;
        for (unsigned off = 0; off + 8 <= it.len && tail < 900; off += 8) {
            uint64_t p = 0; memcpy(&p, b + off, 8);
            if (!IsHeapPtr(p) || p == it.base) continue;
            if (!Readable((void*)p, 64)) continue;
            Item c; c.base = p; c.len = 0x200; c.depth = it.depth + 1;
            snprintf(c.path, sizeof(c.path), "%s+%03x>", it.path, off);
            queue[tail++] = c;
        }
    }
    Log("[gt] chase %d.%d: visited=%d hits=%d\n", testId, sample, visited, hits);
}

// Bulldozer classification. Ships EDEMO or CDEMO, or stashes a CONUP /
// STOPXDEL for the Add hook; the caller arms the cancel only when something
// shipped, and nothing ships that could not be decoded ("never cancel on a
// failed decode" applies doubly to a removal).
// UI::Bulldozer::Apply calls BuildProposal with r8 =
// construction_builder_util::Proposal* (0x2f8 B) whose StreetProposal is its
// FIRST member, and r9 = a 0x70-byte options struct that is NOT proposal-0x70
// for this caller (r9_analysis_dem.md 3, DECOMPILED Bulldozer_Apply.sig.c) --
// everything must be addressed from r8; reading r9+anything here is garbage.
// Offsets relied on:
//   r8+0x30  removedNodes,   24-B records {x,y,z @+0x00, entity @+0x14}
//            (r9 1, DECOMPILED MakeProposalRemove; the earlier "600 B demolish
//            at a2+0x30" was 25 such records)
//   r8+0x48  removedSegments, 120-B SegmentAndEntity {entity @+0x00,
//            node0/node1 @+0x08/+0x0c} (r9 1, DECOMPILED
//            StreetProposal_RemoveSegment; the earlier "30 removals" was ONE
//            120-B record read at a 4-byte stride; node fields INFERRED from
//            addedSegments, sweep test 4 confirms)
//   r8+0x1e0 toRemove vector<Entity>, r8+0x1f8 toAdd stride 0x8e0
//            (r9 2, decompile only -- UNVERIFIED by any sweep)
static bool StashStopDelFromBulldoze(uint64_t eb, int re, uint64_t adb, int aedges);   // defined with the STOPX writers below
static bool ClassifyBulldoze(uint64_t r8)
{
    bool shipped = false;
    // Every vector at the length the bulldozer holds (a drag over a whole
    // district removes hundreds of records in ONE command); only the
    // misread guard applies. toRemove is the construction id list.
    uint64_t nb = 0, eb = 0;
    uint64_t nspan = ReadVecAny(r8 + 0x30, &nb, "BULLDOZE removedNodes");
    uint64_t espan = ReadVecAny(r8 + 0x48, &eb, "BULLDOZE removedSegments");
    std::vector<int32_t> toRemove;
    const bool tok = ReadIntVec(r8 + 0x1e0, &toRemove, "BULLDOZE toRemove");
    int rn = (int)(nspan / 24), re = (int)(espan / 120), nrem = (int)toRemove.size();
    // addedSegments (r8+0x18, 120-B records): a bulldoze that ADDS an edge
    // is an edge REPLACE, not a demolish -- the bulldozer removes a stop or
    // a signal by re-adding the same edge without the object.
    uint64_t adb = 0;
    int aedges = (int)(ReadVecAny(r8 + 0x18, &adb, "BULLDOZE addedSegments") / 120);
    int nadd = 0;
    if (Readable((void*)(r8 + 0x1f8), 16)) {
        uint64_t ab = 0, ae = 0;
        memcpy(&ab, (void*)(r8 + 0x1f8), 8);
        memcpy(&ae, (void*)(r8 + 0x200), 8);
        if (ae > ab) nadd = (int)((ae - ab) / 0x8e0);
    }
    Log("[slice] BULLDOZE rn=%d re=%d toRemove=%d toAdd=%d "
        "(toRemove/toAdd offsets UNVERIFIED -- decompile only)\n",
        rn, re, nrem, nadd);
    if (nspan % 24)
        Log("[slice]   removedNodes span=%llu not a multiple of 24\n",
            (unsigned long long)nspan);
    if (espan % 120)
        Log("[slice]   removedSegments span=%llu not a multiple of 120\n",
            (unsigned long long)espan);
    if (!tok)
        Log("[slice]   toRemove vector unreadable or malformed -- classified without it\n");
    if (nrem >= 1 && nadd >= 1) {
        Log("[slice]   UPGRADE-shaped (toRemove+toAdd) -- module edit\n");
        // STRICT: stash the new CE and arm; CONUP ships from the Add hook if
        // the cancel lands. Undecodable -> the native upgrade runs and a
        // NATIVE notice asks the mod for a catch-up scan.
        if (StashConupFromProposal(r8)) {
            InterlockedExchange(&g_pendingIsConu, 1);
            shipped = true;
        } else {
            Log("[slice]   upgrade params not readable -- NOT cancelled, the mod's catch-up scan ships it\n");
            if (SessionLive()) WriteNativeNotice("upgrade");
        }
    }
    else if (nrem >= 1) {
        Log("[slice]   construction-demolish shape\n");
        // STRICT: ship the ids and let the arm block in DeferHandler cancel
        // the bulldoze exactly as it does for a road. Not shipped (an id
        // that is no entity, no instance letter, the inject file) -> the
        // bulldoze runs natively HERE, so the peers get a NATIVE notice and
        // the catch-up scan sees the constructions gone; without it nothing
        // said the demolish happened at all.
        shipped = WriteCondemoInject(toRemove);
        if (!shipped && SessionLive()) {
            Log("[slice]   CDEMO not shipped -- the bulldoze runs natively, NATIVE notice written\n");
            WriteNativeNotice("construction");
        }
    }
    else if (re >= 1 && aedges >= 1) {
        // An edge object (stop / signal) removed: the edge is re-added without
        // it. Never an EDEMO -- shipped as one (2026-09-08) the replay removed
        // the edge outright, the engine asserted on the object a line still
        // referenced, and every instance wrote a minidump.
        Log("[slice]   edge-REPLACE shape (re=%d addEdges=%d): an edge object removed, not a road\n", re, aedges);
        // STRICT: name the removed object off the two edge records, arm the
        // cancel, and STOPXDEL ships from the Add hook if it lands -- every
        // instance then removes it at the stamp. Undecodable -> the bulldoze
        // runs natively here and a NATIVE notice asks for a catch-up scan.
        if (StashStopDelFromBulldoze(eb, re, adb, aedges)) {
            InterlockedExchange(&g_pendingIsStopDel, 1);
            shipped = true;
        } else {
            Log("[slice]   (not decodable -- runs natively, the mod's catch-up scan ships it)\n");
            if (SessionLive()) WriteNativeNotice("stop");
        }
    }
    else if (re >= 1 || rn >= 1) {
        Log("[slice]   edge-demolish shape\n");
        // A removal is not self-correcting the way an addition is: a road
        // removed on the wrong instance is destroyed work with nothing to
        // rebuild it from. So the far end matches endpoint POSITION and edge
        // KIND, never an entity id, and the cancel is armed only when the
        // removal actually shipped.
        shipped = WriteBulldozeInject(nb, rn, eb, re);
        // Not shipped (no edge record, no instance letter, the inject file):
        // the demolish runs natively here and only here. Say so to the mod;
        // the catch-up scan does not rebuild roads, but the notice puts the
        // native demolish in every log instead of nowhere.
        if (!shipped && SessionLive()) {
            Log("[slice]   EDEMO not shipped -- the bulldoze runs natively, NATIVE notice written\n");
            WriteNativeNotice("road");
        }
    }
    else
        Log("[slice]   empty removal shape -- nothing decoded\n");
    char line[560];
    if (nspan >= 24) {
        float x, y, z; int32_t nid;
        const uint8_t* b = (const uint8_t*)nb;
        memcpy(&x, b + 0x00, 4); memcpy(&y, b + 0x04, 4);
        memcpy(&z, b + 0x08, 4); memcpy(&nid, b + 0x14, 4);
        int o = snprintf(line, sizeof(line),
                         "[slice]   rmNode[0] pos=(%.2f,%.2f,%.2f) id=%d hex=",
                         x, y, z, nid);
        for (int i = 0; i < 24 && o < (int)sizeof(line) - 4; i++)
            o += snprintf(line + o, sizeof(line) - o, "%02x", b[i]);
        Log("%s\n", line);
    }
    if (espan >= 120) {
        int32_t ent, n0, n1;
        const uint8_t* b = (const uint8_t*)eb;
        memcpy(&ent, b + 0x00, 4);
        memcpy(&n0, b + 0x08, 4);
        memcpy(&n1, b + 0x0c, 4);
        int o = snprintf(line, sizeof(line),
                         "[slice]   rmSeg[0] entity=%d node0=%d node1=%d hex=",
                         ent, n0, n1);
        for (int i = 0; i < 120 && o < (int)sizeof(line) - 4; i++)
            o += snprintf(line + o, sizeof(line) - o, "%02x", b[i]);
        Log("%s\n", line);
    }
    if (nrem >= 1)
        Log("[slice]   toRemove[0]=%d (offset +0x1e0 UNVERIFIED)\n", toRemove[0]);
    return shipped;
}

// The SEH guard around the classifier, which owns vectors of its own (a function
// with a __try may not, C2712). A fault anywhere in the decode: nothing shipped,
// nothing cancelled.
static bool LogBulldoze(uint64_t r8)
{
    __try {
        return ClassifyBulldoze(r8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[slice] BULLDOZE classification faulted -- ignored, build proceeds\n");
        return false;   // never cancel on a failed decode
    }
}
