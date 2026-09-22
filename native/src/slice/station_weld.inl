// station_weld.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// MergeTemplateStreet: a script-built station proposal shaped like the UI's (station_weld.h)


// MERGE: turn a script-built construction proposal into the UI's shape.
//
// Differential dump (2026-08-28, both instances, tools/dumpprop_diff.py):
// the sol2 conversion evaluates the construction template at make time and
// APPENDS its connector to the street vectors -- two nodes (flags 0x7f00, ids
// -100004/-100005, RAW geometry) and one segment carrying the ownership the
// API cannot express: +0x68 = construction entity, +0x70 = player, +0x74 = 1.
// The UI's proposal contains exactly that connector, SNAPPED onto the road
// (its outer node at the split point) and nothing else. Our shipped apron
// (-1 -> -2, flags 0, no owner) is therefore a duplicate that collides.
//
// So, in place and allocation-free: keep the template's nodes and segment
// (the 2272-byte construction blob references their placeholder ids), copy
// the originator's snapped positions and tangents INTO them, re-point every
// other segment from our node ids to the template's, and compact our apron
// and its nodes out of the vectors by moving the end pointers. Capacity is
// untouched, so the vectors free normally.
//
// Node record (24 B): x y z @0, flags u32 @0x0c, type i32 @0x10, id i32 @0x14.
// Segment record (120 B): placeholder id @0, node0 @0x08, node1 @0x0c,
// t0 @0x10, t1 @0x1c, ... construction @0x68, player @0x70, owned @0x74.
static const uint32_t NODE_FLAGS_TEMPLATE = 0x7f00;
#include "station_weld.h"

static bool MergeTemplateStreet(uint64_t r8)
{
    if (MergeStationEndpoint(r8)) return true;
    // v3 (2026-08-28). Ghidra (research-construction-linkage): the construction
    // is tied to its street pieces by INDICES -- ConstructionEntity+0x768
    // frozenNodes = indices into addedNodes, +0x780 segmentsBefore = segment
    // count before the template's edges were appended, plus Proposal+0x170
    // (frozen node indices) and +0x188 (construction edge indices). v2's
    // compaction shifted every index and the apply asserted on
    // 'it != result.result.boundingVolumes.end()'. So: never move a record.
    // Lua now ships ONLY the split node X + the halves + the removal; the
    // template appends its connector (inner, outer -- outer LAST). We point the
    // connector's outer end at X, snap tangents, and drop the outer node by
    // moving the vector end back one record.
    if (!Readable((void*)r8, 0x210)) return false;
    uint64_t nb = 0, ne = 0, sb = 0, se = 0, cb = 0, ce = 0, rb = 0, re = 0, fb = 0, fe = 0;
    memcpy(&nb, (void*)(r8 + 0x00), 8); memcpy(&ne, (void*)(r8 + 0x08), 8);
    memcpy(&sb, (void*)(r8 + 0x18), 8); memcpy(&se, (void*)(r8 + 0x20), 8);
    memcpy(&rb, (void*)(r8 + 0x48), 8); memcpy(&re, (void*)(r8 + 0x50), 8);
    memcpy(&fb, (void*)(r8 + 0x170), 8); memcpy(&fe, (void*)(r8 + 0x178), 8);
    memcpy(&cb, (void*)(r8 + 0x1f8), 8); memcpy(&ce, (void*)(r8 + 0x200), 8);
    if (!IsHeapPtr(cb) || ce <= cb) return false;          // no construction: not ours
    if (!IsHeapPtr(nb) || ne <= nb || !IsHeapPtr(sb) || se <= sb) return false;
    // Whole records only, any count. The old 64-node / 64-segment cap made this
    // and the station weld answer "not ours" to a bigger placement on every
    // instance, so the raw apron was built beside ours. The span bound is the
    // misread-pointer guard (PROPOSAL_SANITY_BYTES), logged when it trips.
    if ((ne - nb) % 24 || (se - sb) % 120 || ne - nb > PROPOSAL_SANITY_BYTES || se - sb > PROPOSAL_SANITY_BYTES) {
        Log("[merge] node/segment vectors span %llu/%llu B -- not whole records or past the misread bound, not ours\n",
            (unsigned long long)(ne - nb), (unsigned long long)(se - sb));
        return false;
    }
    int n = (int)((ne - nb) / 24), m = (int)((se - sb) / 120);
    if (n < 2 || m < 1) return false;
    if (!Readable((void*)nb, (size_t)(ne - nb)) || !Readable((void*)sb, (size_t)(se - sb))) return false;
    uint8_t* N = (uint8_t*)nb;
    uint8_t* S = (uint8_t*)sb;
    auto nodeId  = [&](int i) { int32_t v; memcpy(&v, N + i * 24 + 0x14, 4); return v; };

    // Template nodes = the placeholder endpoints of construction-OWNED segments
    // (+0x74 == 1). Node FLAGS are not a discriminator: for a TRACK template the
    // conversion stamps 0x7f00 on OUR node as well (rail depot dump 2026-08-30:
    // our -1 at index 0 already 0x7f00), so "first 0x7f00 node" saw no nodes of
    // ours and every rail depot replayed with the raw apron beside ours.
    std::vector<uint8_t> isT(n, 0); int nT = 0;
    for (int s = 0; s < m; s++) {
        uint32_t owned; memcpy(&owned, S + s * 120 + 0x74, 4);
        if (owned != 1) continue;
        int32_t a, b; memcpy(&a, S + s * 120 + 0x08, 4); memcpy(&b, S + s * 120 + 0x0c, 4);
        for (int i = 0; i < n; i++)
            if (!isT[i] && (nodeId(i) == a || nodeId(i) == b) && nodeId(i) < 0) { isT[i] = true; nT++; }
    }
    int oursN = n - nT;
    if (nT == 0 || oursN == 0) {
        Log("[merge] nodes=%d segs=%d template=%d ours=%d -- nothing to merge\n", n, m, nT, oursN);
        return false;
    }
    // Template outer = the template node nearest to any of ours. Tolerance 15 m,
    // not 2 m: the peer nudges a split point a few metres along the road when the
    // originator's position would leave a stub (execConX STUB NUDGE), and at 2 m the
    // pairing failed -- "no template node within 2 m of ours" -- so the raw template
    // apron stayed put and the depot's driveway never met the road (2026-08-30, two
    // depots visibly unconnected). The template offers only its inner and outer node,
    // metres apart, so a wider radius still picks the same one.
    int X = -1, Tout = -1; float bestD = 225.0f;
    for (int o = 0; o < n; o++) {
        if (isT[o]) continue;
        float ox, oy; memcpy(&ox, N + o * 24, 4); memcpy(&oy, N + o * 24 + 4, 4);
        for (int t = 0; t < n; t++) {
            if (!isT[t]) continue;
            float tx, ty; memcpy(&tx, N + t * 24, 4); memcpy(&ty, N + t * 24 + 4, 4);
            float d = (ox - tx) * (ox - tx) + (oy - ty) * (oy - ty);
            if (d < bestD) { bestD = d; X = o; Tout = t; }
        }
    }
    if (X < 0) { Log("[merge] no template node within 15 m of ours -- untouched\n"); return false; }

    // ENDPOINT WELD (2026-08-29, road depot at a junction). When the UI snapped
    // the apron's outer node onto an EXISTING node J (t in {0,1}: no split, no
    // halves), its proposal is exactly one node (the mouth, 0x7f00) + one
    // segment mouth->J owned by the construction, frozen=[0]. Lua ships that
    // same pair; the sol2 conversion then appends the template's inner node
    // (at the mouth, d=0), outer node (raw, ~10 m out) and apron. Our node
    // pairs with the INNER here, which is not last, so the split path above
    // refused and the raw apron was built beside ours (peer: two coincident
    // mouth nodes, depot frozen to the dangling stub). Adopt instead: our
    // segment BECOMES the apron (copy the template apron's ownership tail into
    // it), re-point the frozen-node index from the inner's index to ours,
    // segmentsBefore to our segment's index, copy the tag, and drop the
    // template's two nodes + apron -- all LAST records, so no index shifts.
    // (+0x188 construction-edge set is empty in every UI dump: size @+0x198.)
    if (Tout == n - 2 && n >= 3 && isT[n - 1]) {
        int inner = Tout, outer = n - 1;
        int32_t xid = nodeId(X), inId = nodeId(inner), outId = nodeId(outer);
        int a = -1, o = -1; int32_t J = 0;
        for (int s = 0; s < m; s++) {
            int32_t s0, s1; uint32_t owned;
            memcpy(&s0, S + s * 120 + 0x08, 4); memcpy(&s1, S + s * 120 + 0x0c, 4);
            memcpy(&owned, S + s * 120 + 0x74, 4);
            bool isApron = owned == 1 && ((s0 == inId && s1 == outId) || (s0 == outId && s1 == inId));
            if (isApron) { a = (a < 0) ? s : -2; continue; }
            if (owned == 0 && ((s0 == xid && s1 >= 0) || (s1 == xid && s0 >= 0))) {
                if (o < 0) { o = s; J = (s0 == xid) ? s1 : s0; } else o = -2;
            }
        }
        if (a != m - 1 || o < 0) {
            Log("[merge-weld] shape mismatch: apron idx=%d (want last=%d) ourSeg=%d -- refusing\n", a, m - 1, o);
            return false;
        }
        // linkage: Proposal+0x170 frozen node indices, CE+0x768 frozenNodes, CE+0x780 segmentsBefore
        int nf = (IsHeapPtr(fb) && fe > fb) ? (int)((fe - fb) / 4) : 0;
        uint64_t cfb = 0, cfe = 0; int32_t segBefore = -1;
        if (!Readable((void*)(cb + 0x768), 0x20)) { Log("[merge-weld] CE unreadable -- refusing\n"); return false; }
        memcpy(&cfb, (void*)(cb + 0x768), 8); memcpy(&cfe, (void*)(cb + 0x770), 8);
        memcpy(&segBefore, (void*)(cb + 0x780), 4);
        int ncf = (IsHeapPtr(cfb) && cfe > cfb) ? (int)((cfe - cfb) / 4) : 0;
        if (ncf != 1 || segBefore != a) {
            Log("[merge-weld] CE linkage unexpected: frozenNodes n=%d segmentsBefore=%d (apron=%d) -- refusing\n", ncf, segBefore, a);
            return false;
        }
        int32_t cf0; memcpy(&cf0, (void*)cfb, 4);
        if (cf0 != inner) { Log("[merge-weld] CE frozenNodes[0]=%d != inner %d -- refusing\n", cf0, inner); return false; }
        // 1. our segment becomes the apron: ownership tail from the template's record
        memcpy(S + o * 120 + 0x28, S + a * 120 + 0x28, 120 - 0x28);
        // 2. our node carries the template's flags
        { uint32_t fl = NODE_FLAGS_TEMPLATE; memcpy(N + X * 24 + 0x0c, &fl, 4); }
        // 3. frozen-node indices -> ours
        { int32_t xi = X; memcpy((void*)cfb, &xi, 4); }
        for (int i = 0; i < nf; i++) {
            int32_t v; memcpy(&v, (void*)(fb + 4 * i), 4);
            if (v == inner) { int32_t xi = X; memcpy((void*)(fb + 4 * i), &xi, 4); }
        }
        { int32_t sb0 = o; memcpy((void*)(cb + 0x780), &sb0, 4); }
        // 4. segment tags (+0x1c8, 32 B each, parallel to addedSegments): copy, drop last
        uint64_t tb = 0, te = 0;
        memcpy(&tb, (void*)(r8 + 0x1c8), 8); memcpy(&te, (void*)(r8 + 0x1d0), 8);
        if (IsHeapPtr(tb) && te > tb && (te - tb) == (uint64_t)m * 32 && Readable((void*)tb, (size_t)(te - tb))) {
            memcpy((void*)(tb + o * 32), (void*)(tb + a * 32), 32);
            uint64_t nte = tb + (uint64_t)(m - 1) * 32;
            memcpy((void*)(r8 + 0x1d0), &nte, 8);
        } else {
            Log("[merge-weld] tags vector span %llu != %d*32 -- left alone\n", (unsigned long long)(te - tb), m);
        }
        // 5. drop the template's apron (last seg) and its two nodes (last two)
        uint64_t nse = sb + (uint64_t)(m - 1) * 120;
        uint64_t nne = nb + (uint64_t)(n - 2) * 24;
        memcpy((void*)(r8 + 0x20), &nse, 8);
        memcpy((void*)(r8 + 0x08), &nne, 8);
        Log("[merge-weld] done: our seg %d (%d->%d) adopted as apron (owner tail from seg %d), frozen idx %d->%d, "
            "segmentsBefore %d->%d; nodes %d->%d, segs %d->%d\n",
            o, xid, J, a, inner, X, segBefore, o, n, n - 2, m, m - 1);
        return true;
    }
    if (Tout != n - 1) {
        Log("[merge] template outer node is index %d, not last (%d) -- refusing (index shift)\n", Tout, n - 1);
        return false;
    }
    // the frozen-index list must not reference the node we drop
    if (IsHeapPtr(fb) && fe > fb && Readable((void*)fb, (size_t)(fe - fb))) {
        int nf = (int)((fe - fb) / 4);
        for (int i = 0; i < nf; i++) {
            int32_t v; memcpy(&v, (void*)(fb + 4 * i), 4);
            Log("[merge] frozen node index[%d] = %d\n", i, v);
            if (v == Tout) { Log("[merge] frozen list references the outer node -- refusing\n"); return false; }
        }
    }
    int32_t xid = nodeId(X), tid = nodeId(Tout);
    // our nodes get the flags the UI's carry
    for (int o = 0; o < n; o++) { if (isT[o]) continue; uint32_t fl = NODE_FLAGS_TEMPLATE; memcpy(N + o * 24 + 0x0c, &fl, 4); }

    // template segments touching Tout -> X, straight tangents from the inner end
    int repointed = 0;
    for (int s = 0; s < m; s++) {
        int32_t a, b; memcpy(&a, S + s * 120 + 0x08, 4); memcpy(&b, S + s * 120 + 0x0c, 4);
        if (a != tid && b != tid) continue;
        int other = -1;
        int32_t oid = (a == tid) ? b : a;
        for (int i = 0; i < n; i++) if (nodeId(i) == oid) { other = i; break; }
        float px[3], po[3];
        memcpy(px, N + X * 24, 12);
        if (other >= 0) memcpy(po, N + other * 24, 12); else memcpy(po, N + Tout * 24, 12);
        float t[3];
        if (a == tid) { memcpy(S + s * 120 + 0x08, &xid, 4); t[0] = po[0] - px[0]; t[1] = po[1] - px[1]; t[2] = po[2] - px[2]; }
        else          { memcpy(S + s * 120 + 0x0c, &xid, 4); t[0] = px[0] - po[0]; t[1] = px[1] - po[1]; t[2] = px[2] - po[2]; }
        memcpy(S + s * 120 + 0x10, t, 12);
        memcpy(S + s * 120 + 0x1c, t, 12);
        repointed++;
        Log("[merge] template seg %d: %d->%d re-pointed to X=%d, tangent=(%.2f,%.2f,%.2f)\n",
            s, a, b, xid, t[0], t[1], t[2]);
    }
    if (!repointed) { Log("[merge] no template segment touches outer node %d -- untouched\n", tid); return false; }

    // our halves: mirror the UI's split halves (+0x64 = 0x7f00, +0x6c from the removed edge)
    if (IsHeapPtr(rb) && re > rb && (re - rb) == 120 && Readable((void*)rb, 120)) {
        int32_t ref; memcpy(&ref, (void*)(rb + 0x6c), 4);
        for (int s = 0; s < m; s++) {
            int32_t a, b; memcpy(&a, S + s * 120 + 0x08, 4); memcpy(&b, S + s * 120 + 0x0c, 4);
            if (a != xid && b != xid) continue;
            uint32_t owned; memcpy(&owned, S + s * 120 + 0x74, 4);
            if (owned == 1) continue;                      // the template connector
            // A UI half is the ORIGINAL edge's record with new endpoints and
            // tangents: street type, +0x2c, +0x4c and the other non-geometry
            // fields come from the edge being split, NOT the construction.
            // Ours carried the depot's type 29 (and uninitialised bytes) onto
            // a type-16 town road: the diff between the one success and every
            // failure since.
            memcpy(S + s * 120 + 0x28, (void*)(rb + 0x28), 0x64 - 0x28);
            uint32_t fl = NODE_FLAGS_TEMPLATE;
            memcpy(S + s * 120 + 0x64, &fl, 4);
            memcpy(S + s * 120 + 0x6c, &ref, 4);
        }
        int32_t st; memcpy(&st, (void*)(rb + 0x48), 4);
        Log("[merge] halves inherit the split edge's record (+0x28..0x63, streetType=%d), "
            "+0x64=0x7f00, +0x6c=%d\n", st, ref);
    }

    // drop the template's outer node: last record, so nothing shifts
    uint64_t newNe = nb + (uint64_t)(n - 1) * 24;
    memcpy((void*)(r8 + 0x08), &newNe, 8);
    Log("[merge] done: X=%d takes over outer node %d (d=%.2f m); nodes %d->%d, segs %d (unchanged)\n",
        xid, tid, sqrtf(bestD), n, n - 1, m);
    return true;
}

