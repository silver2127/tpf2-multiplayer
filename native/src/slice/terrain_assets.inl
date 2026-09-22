// terrain_assets.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// terrain tools and the asset brush: stash at the factory, inject from the Lua's file

// ---------------------------------------------------------------------------
// TERRAIN TOOLS (2026-09-10): what a terraform or paint commit carries, and a
// dev-only test of building one from a script proposal. Layout from the
// decompile (docs/re/PROPOSALS.md "Terrain grids"); none of it was measured
// before this build:
//   +0x278 Grid<CVec2f> {x0,y0,w,h}, vector of {height, base} cells at +0x288
//   +0x2a0 Grid<uint8>  {x0,y0,w,h}, vector at +0x2b0 (0xff = unchanged)
//   +0x2c8 Grid<bool>   {x0,y0,w,h}, vector<uint32> words at +0x2d8, bit count at +0x2f0
// ---------------------------------------------------------------------------
// A detection limit only: one stroke commits before its grid passes 300,000
// cells (2.4 MB of heights), so a larger span means a bad read.
static const uint64_t TERRAIN_MAX_BYTES = 64ull << 20;
static long g_terrainSeq = 0;

struct TerrainGrid { int32_t x0, y0, w, h; uint64_t begin; uint64_t bytes; };

// Header plus data vector (at +0x10 in every grid). bytes stays 0 for an empty
// vector; false only when a non-empty vector cannot be read.
static bool ReadTerrainGrid(uint64_t at, TerrainGrid* g)
{
    memset(g, 0, sizeof(*g));
    if (!Readable((void*)at, 0x28)) return false;
    memcpy(&g->x0, (void*)at, 16);
    uint64_t b = 0, e = 0;
    memcpy(&b, (void*)(at + 0x10), 8);
    memcpy(&e, (void*)(at + 0x18), 8);
    if (b == e) return true;
    g->bytes = ReadVec(at + 0x10, &g->begin, TERRAIN_MAX_BYTES);
    return g->bytes != 0;
}

static uint64_t Fnv1a64(const uint8_t* p, uint64_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// A terrain edit crosses the wire as base64 text: the inject line, the LSCMD
// token and the peer's inject file are all text, and base64 has no space and no
// '=' before its padding, so it survives decodeCmd's key=value scan.
static const char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Returns a malloc'd, NUL-terminated string of 4*ceil(n/3) characters.
static char* Base64Encode(const uint8_t* p, uint64_t n)
{
    const uint64_t outLen = 4 * ((n + 2) / 3);
    char* out = (char*)malloc((size_t)outLen + 1);
    if (!out) return nullptr;
    uint64_t o = 0;
    for (uint64_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out[o++] = B64_ALPHABET[(v >> 18) & 63];
        out[o++] = B64_ALPHABET[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64_ALPHABET[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64_ALPHABET[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

// Decodes into a malloc'd buffer (length in *outLen). Whitespace is skipped;
// any other character outside the alphabet fails the decode.
static uint8_t* Base64Decode(const uint8_t* s, uint64_t n, uint64_t* outLen)
{
    int8_t rev[256];
    memset(rev, -1, sizeof(rev));
    for (int i = 0; i < 64; i++) rev[(uint8_t)B64_ALPHABET[i]] = (int8_t)i;
    uint8_t* out = (uint8_t*)malloc((size_t)(n / 4 * 3 + 3));
    if (!out) return nullptr;
    uint64_t o = 0;
    uint32_t acc = 0;
    int bitsHeld = 0;
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t c = s[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') break;
        if (rev[c] < 0) { free(out); return nullptr; }
        acc = (acc << 6) | (uint32_t)rev[c];
        bitsHeld += 6;
        if (bitsHeld >= 8) {
            bitsHeld -= 8;
            out[o++] = (uint8_t)(acc >> bitsHeld);
        }
    }
    *outLen = o;
    return out;
}

static void AppendTerrainBlob(uint8_t* buf, uint64_t* p, const TerrainGrid& g)
{
    memcpy(buf + *p, &g.bytes, 8);
    *p += 8;
    if (g.bytes) memcpy(buf + *p, (void*)g.begin, (size_t)g.bytes);
    *p += g.bytes;
}

static bool ReadSsoString(uint64_t sa, std::string* out);   // the CONXP walker's reader
static bool LogTerrainProposal(uint64_t r8, uint64_t r9)
{
    if (!Readable((void*)r8, 0x2f8)) { Log("[terrain] proposal unreadable\n"); return false; }
    const long seq = ++g_terrainSeq;

    // Everything a terrain edit should leave empty, so an unexpected shape shows.
    uint64_t b = 0;
    const uint64_t nodesB = ReadVec(r8 + 0x00, &b, TERRAIN_MAX_BYTES);
    const uint64_t segsB  = ReadVec(r8 + 0x18, &b, TERRAIN_MAX_BYTES);
    const uint64_t rmNB   = ReadVec(r8 + 0x30, &b, TERRAIN_MAX_BYTES);
    const uint64_t rmSB   = ReadVec(r8 + 0x48, &b, TERRAIN_MAX_BYTES);
    const uint64_t toRmB  = ReadVec(r8 + 0x1e0, &b, TERRAIN_MAX_BYTES);
    const uint64_t toAddB = ReadVec(r8 + 0x1f8, &b, TERRAIN_MAX_BYTES);
    const uint64_t v250B  = ReadVec(r8 + 0x250, &b, TERRAIN_MAX_BYTES);
    uint64_t set188 = 0, old2new = 0, map268 = 0, bits = 0;
    memcpy(&set188, (void*)(r8 + 0x198), 8);      // unordered_set size (list size at +0x10)
    memcpy(&old2new, (void*)(r8 + 0x220), 8);     // unordered_map size
    memcpy(&map268, (void*)(r8 + 0x270), 8);      // std::map size
    memcpy(&bits, (void*)(r8 + 0x2f0), 8);
    Log("[terrain] #%ld ProposalAction commit: nodes=%lluB segs=%lluB rmNodes=%lluB rmSegs=%lluB "
        "set188=%llu toRemove=%lluB toAdd=%lluB old2new=%llu v250=%lluB map268=%llu\n",
        seq, (unsigned long long)nodesB, (unsigned long long)segsB, (unsigned long long)rmNB,
        (unsigned long long)rmSB, (unsigned long long)set188, (unsigned long long)toRmB,
        (unsigned long long)toAddB, (unsigned long long)old2new, (unsigned long long)v250B,
        (unsigned long long)map268);

    // An asset-brush commit (toAdd / toRemove, no grids) is decoded by
    // StashAssetsFromProposal, called from the factory branch.
    if (v250B >= 4) {
        uint64_t vb = 0;
        ReadVec(r8 + 0x250, &vb, TERRAIN_MAX_BYTES);
        char s[200] = "";
        int o = 0;
        for (uint64_t i = 0; i < v250B / 4 && i < 16 && vb; i++) {
            int32_t v;
            memcpy(&v, (void*)(vb + i * 4), 4);
            o += snprintf(s + o, sizeof(s) - o, " %d", v);
        }
        Log("[terrain] #%ld   v250 (%llu ints):%s\n", seq, (unsigned long long)(v250B / 4), s);
    }

    TerrainGrid hg, mg, kg;
    const bool hok = ReadTerrainGrid(r8 + 0x278, &hg);
    const bool mok = ReadTerrainGrid(r8 + 0x2a0, &mg);
    const bool kok = ReadTerrainGrid(r8 + 0x2c8, &kg);
    Log("[terrain] #%ld heights%s x0=%d y0=%d w=%d h=%d data=%lluB (w*h*8=%lld)\n", seq,
        hok ? "" : " UNREADABLE", hg.x0, hg.y0, hg.w, hg.h, (unsigned long long)hg.bytes,
        (long long)hg.w * hg.h * 8);
    Log("[terrain] #%ld material%s x0=%d y0=%d w=%d h=%d data=%lluB (w*h=%lld)\n", seq,
        mok ? "" : " UNREADABLE", mg.x0, mg.y0, mg.w, mg.h, (unsigned long long)mg.bytes,
        (long long)mg.w * mg.h);
    Log("[terrain] #%ld mask%s x0=%d y0=%d w=%d h=%d words=%lluB bits=%llu (w*h=%lld)\n", seq,
        kok ? "" : " UNREADABLE", kg.x0, kg.y0, kg.w, kg.h, (unsigned long long)kg.bytes,
        (unsigned long long)bits, (long long)kg.w * kg.h);

    if (hok && hg.w > 0 && hg.h > 0 && hg.bytes == (uint64_t)hg.w * (uint64_t)hg.h * 8) {
        const float* c = (const float*)hg.begin;
        const uint64_t n = hg.bytes / 8;
        uint64_t changed = 0;
        float hmin = c[0], hmax = c[0], bmin = c[1], bmax = c[1], dmin = 0, dmax = 0;
        for (uint64_t i = 0; i < n; i++) {
            const float hv = c[2 * i], bv = c[2 * i + 1];
            if (hv < hmin) hmin = hv;
            if (hv > hmax) hmax = hv;
            if (bv < bmin) bmin = bv;
            if (bv > bmax) bmax = bv;
            if (hv != bv) {
                changed++;
                if (hv - bv < dmin) dmin = hv - bv;
                if (hv - bv > dmax) dmax = hv - bv;
            }
        }
        const uint64_t ci = (uint64_t)(hg.h / 2) * (uint64_t)hg.w + (uint64_t)(hg.w / 2);
        Log("[terrain] #%ld heights: %llu cells, %llu changed; height %.3f..%.3f, base %.3f..%.3f, "
            "delta %.3f..%.3f; centre (%d,%d) = {%.4f, %.4f}; fnv=%016llx\n",
            seq, (unsigned long long)n, (unsigned long long)changed, hmin, hmax, bmin, bmax, dmin, dmax,
            hg.x0 + hg.w / 2, hg.y0 + hg.h / 2, c[2 * ci], c[2 * ci + 1],
            (unsigned long long)Fnv1a64((const uint8_t*)hg.begin, hg.bytes));
    }
    if (mok && mg.w > 0 && mg.h > 0 && mg.bytes == (uint64_t)mg.w * (uint64_t)mg.h) {
        const uint8_t* m = (const uint8_t*)mg.begin;
        uint64_t hist[256] = {};
        for (uint64_t i = 0; i < mg.bytes; i++) hist[m[i]]++;
        char vals[200] = "";
        int o = 0, shown = 0;
        for (int v = 0; v < 255 && shown < 6; v++) {
            if (!hist[v]) continue;
            o += snprintf(vals + o, sizeof(vals) - o, " %d:%llu", v, (unsigned long long)hist[v]);
            shown++;
        }
        Log("[terrain] #%ld material: %llu cells, %llu unchanged (0xff), painted:%s; fnv=%016llx\n",
            seq, (unsigned long long)mg.bytes, (unsigned long long)hist[255], shown ? vals : " none",
            (unsigned long long)Fnv1a64(m, mg.bytes));
    }
    if (kok && kg.bytes >= 4 && bits <= kg.bytes * 8) {
        const uint32_t* w = (const uint32_t*)kg.begin;
        uint64_t set = 0;
        for (uint64_t i = 0; i < bits; i++) set += (w[i >> 5] >> (i & 31)) & 1;
        Log("[terrain] #%ld mask: %llu of %llu bits set\n", seq, (unsigned long long)set,
            (unsigned long long)bits);
    }
    if (Readable((void*)r9, 0x70))
        GtDumpRange("TCTX_", 0, (int)seq, (const uint8_t*)r9, 0x70, 0);

    // The edit as one TPTG blob: "TPTG", u32 version 1, the 0x80-byte grid tail
    // from +0x278, the 0x70-byte context, then heights, material and mask as
    // u64 size + data. tools\re\terrain_bin.py reads it; InjectTerrainFromFile
    // puts it back into a proposal.
    const bool gridsOk = hok && mok && kok &&
        hg.bytes == (uint64_t)hg.w * (uint64_t)hg.h * 8 &&
        mg.bytes == (uint64_t)mg.w * (uint64_t)mg.h &&
        (uint64_t)kg.w * (uint64_t)kg.h == bits && kg.bytes == ((bits + 31) / 32) * 4;
    const bool hasEdit = hg.bytes || mg.bytes;
    const bool onlyTerrain = !nodesB && !segsB && !rmNB && !rmSB && !toRmB && !toAddB;
    const bool ship = SessionLive() && hasEdit;
    if (!ship && !DumpPropOn()) return false;
    if (!g_dataDir[0]) return false;

    const uint64_t blobLen = 8 + 0x80 + 0x70 + 24 + hg.bytes + mg.bytes + kg.bytes;
    uint8_t* blob = (uint8_t*)malloc((size_t)blobLen);
    if (!blob) { Log("[terrain] #%ld out of memory for a %lluB edit\n", seq, (unsigned long long)blobLen); return false; }
    {
        const uint32_t ver = 1;
        uint64_t p = 0;
        memcpy(blob, "TPTG", 4);
        memcpy(blob + 4, &ver, 4);
        memcpy(blob + 8, (void*)(r8 + 0x278), 0x80);
        memset(blob + 8 + 0x80, 0, 0x70);
        if (Readable((void*)r9, 0x70)) memcpy(blob + 8 + 0x80, (void*)r9, 0x70);
        p = 8 + 0x80 + 0x70;
        AppendTerrainBlob(blob, &p, hg);
        AppendTerrainBlob(blob, &p, mg);
        AppendTerrainBlob(blob, &p, kg);
    }

    // The grids as a file, for tools\re, only with dumpprop: a painting session
    // commits on every release.
    if (DumpPropOn()) {
        char name[80], path[MAX_PATH];
        snprintf(name, sizeof(name), "terrain_%s_%lu_%03ld.bin", g_instance,
                 (unsigned long)GetCurrentProcessId(), seq);
        snprintf(path, sizeof(path), "%s%s", g_dataDir, name);
        FILE* f = _fsopen(path, "wb", _SH_DENYWR);
        if (f) {
            fwrite(blob, 1, (size_t)blobLen, f);
            fclose(f);
            Log("[terrain] #%ld saved %s\n", seq, name);
        } else {
            Log("[terrain] #%ld could not create %s\n", seq, name);
        }
    }

    // REPLICATION, STRICT (2026-09-11). The blob is STASHED here; the factory
    // arms the cancel and the Add hook writes TERRAINCAP once it knows whether
    // the cancel landed (ARMED 1: every instance, this one included, applies
    // the grids at the stamp through the empty-carrier replay) or the edit had
    // to run natively here (ARMED 0: the peers apply it, this instance keeps
    // its native copy -- the v1 behaviour, now the fallback). Heights and
    // material are absolute, so a replay onto an identical world lands
    // bit-identical; a terrain that changes EXEC_DELAY earlier on one instance
    // is exactly the kind of window a later build reads a different height from.
    bool stashed = false;
    if (ship) {
        if (!gridsOk) {
            Log("[terrain] #%ld grid sizes do not match their data -- the edit runs here only, NOT replicated\n", seq);
        } else if (!onlyTerrain) {
            Log("[terrain] #%ld carries streets or constructions as well -- NOT replicated as terrain\n", seq);
        } else {
            char* b64 = Base64Encode(blob, blobLen);
            if (b64) {
                free(g_terrainB64);
                g_terrainB64 = b64; g_terrainBlobLen = blobLen; g_terrainStashSeq = seq;
                // paint only: material and mask, no heights (see the factory branch)
                g_terrainIsPaint = (hg.bytes == 0 && mg.bytes != 0);
                stashed = true;
                Log("[terrain] #%ld stashed for the wire: %lluB edit, %lluB of base64\n",
                    seq, (unsigned long long)blobLen, (unsigned long long)strlen(b64));
            } else {
                Log("[terrain] #%ld base64 encode failed -- the edit runs here only, NOT replicated\n", seq);
            }
        }
    }
    free(blob);
    return stashed;
}

// Write the stashed edit as TERRAINCAP behind ARMED <armed>. armed=true: the
// cancel landed, the originator replays too. armed=false: the edit ran natively
// here (no live session at arm time, or the callback could not be fired), the
// mod ships it with skipOrigin so the peers still get it.
static void HoldTerrainTool(uint64_t impl)
{
    uint64_t tool = 0;
    if (Readable((void*)(impl + 8), 8)) memcpy(&tool, (void*)(impl + 8), 8);
    if (!tool || !Readable((void*)(tool + 0xf0), 1)) {
        Log("[terrain] cannot hold the tool (impl=%llx tool=%llx) -- the stroke resumes before the replay\n", (unsigned long long)impl, (unsigned long long)tool);
        return;
    }
    *(volatile uint8_t*)(tool + 0xf0) = 1;
    g_terrainHeldTool = tool; g_terrainHeldAt = GetTickCount64();
    Log("[terrain] tool %llx held (+0xf0) until our replay carrier is added\n", (unsigned long long)tool);
}

// True for a proposal with no nodes, segments, removals or constructions --
// what terrain.lua's completion marker looks like (its carrier had grids
// injected; the marker gets nothing, there is no file left to inject).
static bool ProposalIsEmpty(uint64_t r8)
{
    uint64_t b = 0;
    static const uint64_t offs[] = { 0x00, 0x18, 0x30, 0x48, 0x1e0 };
    for (int i = 0; i < 5; i++) {
        if (ReadVec(r8 + offs[i], &b, 0x20000)) return false;
    }
    uint64_t ab = 0, ae = 0;
    if (Readable((void*)(r8 + 0x1f8), 16)) { memcpy(&ab, (void*)(r8 + 0x1f8), 8); memcpy(&ae, (void*)(r8 + 0x200), 8); }
    return ae <= ab;
}

static void ReleaseTerrainTool(const char* why)
{
    uint64_t tool = g_terrainHeldTool;
    if (!tool) return;
    g_terrainHeldTool = 0;
    if (Readable((void*)(tool + 0xf0), 1)) *(volatile uint8_t*)(tool + 0xf0) = 0;
    Log("[terrain] tool %llx released after %llu ms (%s)\n", (unsigned long long)tool, (unsigned long long)(GetTickCount64() - g_terrainHeldAt), why);
}

static void WriteInjectTerrain(bool armed)
{
    ReadInstance();
    char* b64 = g_terrainB64; g_terrainB64 = nullptr;
    if (!b64) return;
    if (!g_instance[0]) { free(b64); return; }
    WriteArmed(armed);
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (f) {
        // one write for the payload: the reader re-reads a partial line
        fprintf(f, "TERRAINCAP %llu ", (unsigned long long)g_terrainBlobLen);
        fwrite(b64, 1, strlen(b64), f);
        fputc('\n', f);
        fclose(f);
        Log("[terrain] #%ld shipped: %lluB edit, %lluB of base64 (%s)\n", g_terrainStashSeq,
            (unsigned long long)g_terrainBlobLen, (unsigned long long)strlen(b64),
            armed ? "cancelled here, every instance applies it at the stamp" : "ran natively here, the peers apply it at the stamp");
    } else {
        Log("[terrain] #%ld cannot open %s -- the edit is on this instance only, NOT replicated\n", g_terrainStashSeq, p);
    }
    free(b64);
}

// DEV TEST, inert unless terrain_inject_<inst>.bin exists in the data dir: a
// script-built proposal that carries nothing (api.cmd.make.buildProposal with
// an empty SimpleProposal) gets the grids of that file, a capture saved above.
// It answers what the decompile cannot: does the engine build a terrain edit
// that arrives this way. The file is deleted once read, usable or not.
static const uintptr_t RVA_VECCOPY_8 = 0x0cc990;   // vector<8-byte> copy ctor (game allocator)
static const uintptr_t RVA_VECCOPY_1 = 0x1ded10;   // vector<uint8>
static const uintptr_t RVA_VECCOPY_4 = 0x125480;   // vector<uint32>
using GameVecCopy = uint64_t* (*)(uint64_t* dst, const uint64_t* src, uint64_t, uint64_t);

static bool TerrainCarrierEmpty(uint64_t r8)
{
    if (!Readable((void*)r8, 0x2f8)) return false;
    // street half, construction fields, and the three grid vectors
    static const unsigned vecs[] = { 0x00, 0x18, 0x30, 0x48, 0xf8, 0x1c8, 0x1e0, 0x1f8, 0x250 };
    for (unsigned off : vecs) {
        uint64_t b = 0, e = 0;
        memcpy(&b, (void*)(r8 + off), 8);
        memcpy(&e, (void*)(r8 + off + 8), 8);
        if (b != e) return false;
    }
    // no allocation to leak: the game's copy constructors overwrite without freeing
    static const unsigned grids[] = { 0x278, 0x2a0, 0x2c8 };
    for (unsigned off : grids) {
        uint64_t b = 0;
        memcpy(&b, (void*)(r8 + off + 0x10), 8);
        if (b != 0) return false;
    }
    return true;
}

static bool InjectTerrainFromFile(uint64_t r8)
{
    if (!g_dataDir[0]) return false;
    // NOT the letter from attach: the lobby renames a joiner after the slice
    // loads, and a peer that never edits calls nothing else that re-reads it.
    // B attached as "a", looked for terrain_inject_a.bin, and left every
    // terrain_inject_b.bin unread -- no terraform or paint reached it (2026-09-11).
    ReadInstance();
    if (!g_instance[0]) return false;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sterrain_inject_%s.bin", g_dataDir, g_instance);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return false;
    if (!TerrainCarrierEmpty(r8)) {
        Log("[terrain-inject] inject file present, but this script proposal is not empty -- left alone\n");
        return false;
    }
    FILE* f = _fsopen(path, "rb", _SH_DENYNO);
    if (!f) { Log("[terrain-inject] inject file present but not readable\n"); return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    const long minLen = 8 + 0x80 + 0x70 + 24;
    uint8_t* buf = (len >= minLen && (uint64_t)len <= TERRAIN_MAX_BYTES) ? (uint8_t*)malloc((size_t)len) : nullptr;
    bool got = buf && fread(buf, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    DeleteFileA(path);
    // The mod writes the TERRAIN command's payload as the base64 it arrived in;
    // a raw TPTG capture (the dev test) is used as it is.
    long used = len;
    if (got && memcmp(buf, "TPTG", 4) != 0) {
        uint64_t rawLen = 0;
        uint8_t* raw = Base64Decode(buf, (uint64_t)len, &rawLen);
        free(buf);
        buf = raw;
        used = (long)rawLen;
        got = raw && used >= minLen;
    }
    if (!got) {
        free(buf);
        Log("[terrain-inject] inject file too short, too long or unreadable (%ld B) -- left alone\n", len);
        return false;
    }
    len = used;

    bool done = false;
    do {
        if (memcmp(buf, "TPTG", 4) != 0) { Log("[terrain-inject] not a TPTG file -- left alone\n"); break; }
        const uint8_t* tail = buf + 8;
        uint64_t p = 8 + 0x80 + 0x70, n[3] = {};
        const uint8_t* data[3] = {};
        bool truncated = false;
        for (int i = 0; i < 3; i++) {
            if (p + 8 > (uint64_t)len) { truncated = true; break; }
            memcpy(&n[i], buf + p, 8);
            p += 8;
            if (n[i] > (uint64_t)len - p) { truncated = true; break; }
            data[i] = buf + p;
            p += n[i];
        }
        if (truncated) { Log("[terrain-inject] truncated file -- left alone\n"); break; }
        int32_t hd[4], md[4], kd[4];
        uint64_t bits = 0;
        memcpy(hd, tail + 0x00, 16);
        memcpy(md, tail + 0x28, 16);
        memcpy(kd, tail + 0x50, 16);
        memcpy(&bits, tail + 0x78, 8);
        if (hd[2] < 0 || hd[3] < 0 || md[2] < 0 || md[3] < 0 || kd[2] < 0 || kd[3] < 0 ||
            (uint64_t)hd[2] * (uint64_t)hd[3] * 8 != n[0] ||
            (uint64_t)md[2] * (uint64_t)md[3] != n[1] ||
            (uint64_t)kd[2] * (uint64_t)kd[3] != bits || n[2] != ((bits + 31) / 32) * 4) {
            Log("[terrain-inject] grid sizes do not match their data (heights %dx%d/%lluB, material %dx%d/%lluB, "
                "mask %dx%d/%llu bits/%lluB) -- left alone\n", hd[2], hd[3], (unsigned long long)n[0],
                md[2], md[3], (unsigned long long)n[1], kd[2], kd[3], (unsigned long long)bits,
                (unsigned long long)n[2]);
            break;
        }
        uint64_t src[3];
        if (n[0]) {
            src[0] = (uint64_t)data[0]; src[1] = src[0] + n[0]; src[2] = src[1];
            ((GameVecCopy)(g_base + RVA_VECCOPY_8))((uint64_t*)(r8 + 0x288), src, 0, 0);
        }
        if (n[1]) {
            src[0] = (uint64_t)data[1]; src[1] = src[0] + n[1]; src[2] = src[1];
            ((GameVecCopy)(g_base + RVA_VECCOPY_1))((uint64_t*)(r8 + 0x2b0), src, 0, 0);
        }
        if (n[2]) {
            src[0] = (uint64_t)data[2]; src[1] = src[0] + n[2]; src[2] = src[1];
            ((GameVecCopy)(g_base + RVA_VECCOPY_4))((uint64_t*)(r8 + 0x2d8), src, 0, 0);
        }
        memcpy((void*)(r8 + 0x278), hd, 16);
        memcpy((void*)(r8 + 0x2a0), md, 16);
        memcpy((void*)(r8 + 0x2c8), kd, 16);
        memcpy((void*)(r8 + 0x2f0), &bits, 8);
        Log("[terrain-inject] filled the script proposal: heights %dx%d at (%d,%d), material %dx%d at (%d,%d), "
            "mask %llu bits\n", hd[2], hd[3], hd[0], hd[1], md[2], md[3], md[0], md[1],
            (unsigned long long)bits);
        done = true;
    } while (0);
    free(buf);
    return done;
}

// ---------------------------------------------------------------------------
// ASSET BRUSH (2026-09-11). DECOMPILED: UI::AssetBrush (vftable 0x2fbdd20)
// builds its proposal in MakeBuildAssetsProposal 0x3d1aa0 (paint) or 0x3d3280
// (erase), the same records CreateProposalAddAsset 0xa13fc0 makes. MEASURED
// (asset probe, A, 9 strokes): every toAdd record reads fileName '', params {}
// and transf identity -- the known ConstructionEntity fields are defaults.
//   toRemove +0x1e0  vector<int>   the existing groups the stroke touched
//   toAdd    +0x1f8  vector<CE>    one 0x8e0 record per group:
//                                  +0x020 int 0xb, +0x20d byte 1,
//                                  +0x550 vector of one 0x48 record {.., 0.75f @+0x38, 2.5f @+0x3c},
//                                  +0x470 vector<TransformedModel>
//   TransformedModel 0x80           +0x00 std::string model, +0x20 std::string, +0x40 Mat4f
// A touched group is removed and re-added with the models it keeps; an erase
// that empties a group removes it with no record. Nothing random is left to
// recompute: model, rotation and scale are baked into each matrix.
//
// Lua cannot build these records (the SimpleProposal ConstructionEntity has no
// +0x20 or +0x470), so the replay is native, like the terrain grids: the
// originator ships every record's models, and at the stamp every instance fills
// an empty script proposal with them through the game's own constructors and
// vector operations. Removed groups travel as positions (the mod resolves the
// ids while they still stand -- the stroke is cancelled here), never as ids.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_CE_CTOR       = 0x3ceae0;  // ConstructionEntity::ConstructionEntity()
static const uintptr_t RVA_CE_DTOR       = 0x3d0430;  // ~ConstructionEntity()
static const uintptr_t RVA_CE_COPY_AT    = 0x3ce460;  // copy-construct a CE at (dst, const CE&)
static const uintptr_t RVA_VEC_CE_GROW   = 0x3c8680;  // vector<CE>::_Emplace_reallocate(vec, where, const CE&)
static const uintptr_t RVA_VEC_48_GROW   = 0x3c8b40;  // vector<0x48 record>::_Emplace_reallocate(vec, where, rec&&)
static const uintptr_t RVA_VEC_TM_ASSIGN = 0x3c7780;  // vector<TransformedModel>::assign(vec, first, last)
static const uintptr_t RVA_VEC_INT_GROW  = 0x0e8060;  // vector<int>::_Emplace_reallocate(vec, where, const int&)
static const int32_t   ASSET_GROUP_TYPE  = 0xb;
// Wire version 2 (2026-09-16): string lengths are u32 and no count has a cap.
// v1 capped a stroke at 4096 groups of 20000 models with 511-byte strings, and
// an over-cap stroke ran on the originator only -- the town-growth desync the
// asset brush was known for. Both ends of the wire are this DLL (the lobby
// gates on the exact version), so v1 is simply refused.
static const uint32_t  ASSET_WIRE_VERSION = 2;

// The stroke blob, in memory, as big as the stroke.
static void AbPut(std::vector<uint8_t>* b, const void* d, uint64_t len)
{
    const uint8_t* p = (const uint8_t*)d;
    b->insert(b->end(), p, p + (size_t)len);
}

// An asset-brush stroke off the ProposalAction commit: "TPAS", u32 version 2,
// u32 records, u32 removals, then per record u32 models and per model
// u32 + model path, u32 + second string, 64 bytes of Mat4f. Stashed as base64
// with the removed group ids; false (and nothing stashed) for anything that is
// not purely an asset stroke or does not read cleanly -- never ship bad data.
static bool StashAssetsFromProposal(uint64_t r8, long seq)
{
    if (!Readable((void*)r8, 0x2f8)) return false;
    uint64_t ab = 0, rb = 0, b = 0;
    const uint64_t toAddB = ReadVecLoud(r8 + 0x1f8, &ab, "asset toAdd");
    const uint64_t toRmB  = ReadVecLoud(r8 + 0x1e0, &rb, "asset toRemove");
    if (!toAddB && !toRmB) return false;
    // a stroke touches nothing else: no street half, no grids
    if (ReadVec(r8 + 0x00, &b, PROPOSAL_SANITY_BYTES) || ReadVec(r8 + 0x18, &b, PROPOSAL_SANITY_BYTES) ||
        ReadVec(r8 + 0x30, &b, PROPOSAL_SANITY_BYTES) || ReadVec(r8 + 0x48, &b, PROPOSAL_SANITY_BYTES) ||
        ReadVec(r8 + 0x288, &b, PROPOSAL_SANITY_BYTES) || ReadVec(r8 + 0x2b0, &b, PROPOSAL_SANITY_BYTES) ||
        ReadVec(r8 + 0x2d8, &b, PROPOSAL_SANITY_BYTES))
        return false;
    if (toAddB % 0x8e0 || toRmB % 4) {
        Log("[asset] #%ld toAdd %lluB / toRemove %lluB do not divide into records -- not an asset stroke\n",
            seq, (unsigned long long)toAddB, (unsigned long long)toRmB);
        return false;
    }
    const uint32_t nrec = (uint32_t)(toAddB / 0x8e0);
    const uint32_t nrm  = (uint32_t)(toRmB / 4);

    std::vector<uint8_t> out;
    std::string ids, first;
    uint64_t models = 0;
    float fx = 0, fy = 0, fz = 0;
    const char* why = nullptr;
    try {
        // the removed ids, as text for the mod (it turns them into positions)
        for (uint32_t i = 0; i < nrm; i++) {
            int32_t id = 0;
            memcpy(&id, (void*)(rb + (uint64_t)i * 4), 4);
            if (i) ids.push_back(',');
            ids.append(std::to_string(id));
        }
        AbPut(&out, "TPAS", 4); AbPut(&out, &ASSET_WIRE_VERSION, 4); AbPut(&out, &nrec, 4); AbPut(&out, &nrm, 4);
        std::string s1, s2;
        for (uint32_t i = 0; i < nrec && !why; i++) {
            const uint64_t ce = ab + (uint64_t)i * 0x8e0;
            if (!Readable((void*)ce, 0x8e0)) { why = "a record is unreadable"; break; }
            int32_t type = 0;
            memcpy(&type, (void*)(ce + 0x20), 4);
            if (type != ASSET_GROUP_TYPE) { why = "a record is not an asset group (type != 0xb)"; break; }
            uint64_t mb = 0, me = 0;
            memcpy(&mb, (void*)(ce + 0x470), 8);
            memcpy(&me, (void*)(ce + 0x478), 8);
            if (me > mb && me - mb > PROPOSAL_SANITY_BYTES) {
                Log("[asset] #%ld record %u: model vector spans %llu B -- past the misread-pointer bound\n",
                    seq, i, (unsigned long long)(me - mb));
                why = "a record's model list is a misread pointer"; break;
            }
            if (me < mb || (me - mb) % 0x80 || me == mb ||
                !Readable((void*)mb, (size_t)(me - mb))) { why = "a record's model list does not read"; break; }
            const uint32_t nm = (uint32_t)((me - mb) / 0x80);
            AbPut(&out, &nm, 4);
            for (uint32_t k = 0; k < nm; k++) {
                const uint64_t tm = mb + (uint64_t)k * 0x80;
                if (!ReadSsoString(tm, &s1) || s1.empty() || !ReadSsoString(tm + 0x20, &s2)) {
                    why = "a model's strings do not read"; break;
                }
                const uint32_t w1 = (uint32_t)s1.size(), w2 = (uint32_t)s2.size();
                AbPut(&out, &w1, 4); AbPut(&out, s1.data(), w1);
                AbPut(&out, &w2, 4); AbPut(&out, s2.data(), w2);
                AbPut(&out, (void*)(tm + 0x40), 0x40);
                if (models == 0) {
                    first = s1;
                    memcpy(&fx, (void*)(tm + 0x70), 4); memcpy(&fy, (void*)(tm + 0x74), 4); memcpy(&fz, (void*)(tm + 0x78), 4);
                }
                models++;
            }
        }
    } catch (const std::bad_alloc&) {
        why = "out of memory stashing the stroke";
    }
    if (why) {
        Log("[asset] #%ld %s -- the stroke runs here only, NOT replicated\n", seq, why);
        return false;
    }
    char* b64 = Base64Encode(out.data(), out.size());
    if (!b64) { Log("[asset] #%ld base64 encode failed -- NOT replicated\n", seq); return false; }
    free(g_assetB64);
    g_assetB64 = b64; g_assetBlobLen = out.size(); g_assetStashSeq = seq;
    g_assetRemoveIds.swap(ids);
    g_assetRemoveCount = (int)nrm;
    Log("[asset] #%ld stashed: %u group(s), %llu model(s), %u removal(s); first '%.200s' at (%.1f,%.1f,%.1f); %lluB\n",
        seq, nrec, (unsigned long long)models, nrm, first.c_str(), fx, fy, fz, (unsigned long long)out.size());
    return true;
}

// ASSETCAP <bytes> <base64> <removals> <id,id,...|->  behind ARMED <armed>.
static void WriteInjectAssets(bool armed)
{
    ReadInstance();
    char* b64 = g_assetB64; g_assetB64 = nullptr;
    if (!b64) return;
    if (!g_instance[0]) { free(b64); return; }
    WriteArmed(armed);
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (f) {
        fprintf(f, "ASSETCAP %llu ", (unsigned long long)g_assetBlobLen);
        fwrite(b64, 1, strlen(b64), f);
        fprintf(f, " %d %s\n", g_assetRemoveCount, g_assetRemoveCount ? g_assetRemoveIds.c_str() : "-");
        fclose(f);
        Log("[asset] #%ld shipped: %lluB stroke, %d removal(s) (%s)\n", g_assetStashSeq,
            (unsigned long long)g_assetBlobLen, g_assetRemoveCount,
            armed ? "cancelled here, every instance applies it at the stamp" : "ran natively here, the peers apply it at the stamp");
    } else {
        Log("[asset] #%ld cannot open %s -- the stroke is on this instance only, NOT replicated\n", g_assetStashSeq, p);
    }
    free(b64);
}

// An MSVC std::string the game's copy constructor can read: SSO below 16 chars,
// otherwise a pointer to our own buffer (freed by us once copied).
static char* PutReadOnlyStdString(uint8_t* at, const std::string& s)
{
    memset(at, 0, 0x20);
    const uint64_t len = s.size();
    uint64_t cap = 15;
    char* heap = nullptr;
    if (len < 16) {
        memcpy(at, s.data(), (size_t)len);
    } else {
        heap = (char*)malloc((size_t)len + 1);
        if (!heap) return (char*)-1;
        memcpy(heap, s.data(), (size_t)len);
        heap[len] = 0;
        memcpy(at, &heap, 8);
        cap = len;
    }
    memcpy(at + 0x10, &len, 8);
    memcpy(at + 0x18, &cap, 8);
    return heap;
}

struct AssetModelSrc { std::string model, extra; uint8_t m[0x40]; };

// The inject file's text, "rm <id,id,...|->\n<base64 TPAS stroke>", into removal
// ids and per-group model lists. Returns why it is unusable, or nullptr. Pure (no
// game memory), so tools\re\asset_stroke_test.py round-trips it offline.
static const char* ParseAssetStroke(const std::string& text, std::vector<int32_t>* rm,
                                    std::vector<std::vector<AssetModelSrc>>* recs)
{
    rm->clear();
    recs->clear();
    const size_t nl = text.find('\n');
    if (text.compare(0, 3, "rm ") != 0 || nl == std::string::npos) return "malformed inject file";
    std::string ids = text.substr(3, nl - 3);
    while (!ids.empty() && (ids.back() == '\r' || ids.back() == ' ')) ids.pop_back();
    if (ids != "-") {
        const char* s = ids.c_str();
        while (*s) {
            char* e = nullptr;
            const long v = strtol(s, &e, 10);
            if (e == s || v <= 0) return "bad removal list";
            rm->push_back((int32_t)v);
            if (*e == ',') s = e + 1;
            else if (*e == 0) s = e;
            else return "bad removal list";
        }
    }
    const std::string b64 = text.substr(nl + 1);
    uint64_t rawLen = 0;
    uint8_t* raw = Base64Decode((const uint8_t*)b64.data(), b64.size(), &rawLen);
    if (!raw) return "payload is not base64";
    const char* bad = nullptr;
    uint64_t p = 0;
    auto take = [&](void* d, uint64_t n) -> bool {
        if (n > rawLen - p) return false;
        memcpy(d, raw + p, (size_t)n);
        p += n;
        return true;
    };
    uint32_t ver = 0, nrec = 0, nrm = 0;
    char magic[4];
    if (!take(magic, 4) || memcmp(magic, "TPAS", 4) != 0) bad = "not a TPAS stroke";
    // A count is bounded by the bytes behind it, never by a constant: a model
    // record is at least 4 + 4 + 64 bytes (an empty path then fails as "bad
    // model path", its own refusal), a group at least 4 + one model.
    const uint64_t MODEL_MIN = 4 + 4 + 0x40;
    if (!bad && (!take(&ver, 4) || ver != ASSET_WIRE_VERSION || !take(&nrec, 4) || !take(&nrm, 4) ||
                 nrec > (rawLen - p) / (4 + MODEL_MIN))) bad = "bad header";
    for (uint32_t i = 0; i < nrec && !bad; i++) {
        uint32_t nm = 0;
        if (!take(&nm, 4) || nm == 0 || nm > (rawLen - p) / MODEL_MIN) { bad = "bad model count"; break; }
        std::vector<AssetModelSrc> models(nm);
        for (uint32_t k = 0; k < nm && !bad; k++) {
            uint32_t l1 = 0, l2 = 0;
            if (!take(&l1, 4) || l1 == 0 || l1 > rawLen - p) { bad = "bad model path"; break; }
            models[k].model.assign((const char*)raw + p, l1);
            p += l1;
            if (!take(&l2, 4) || l2 > rawLen - p) { bad = "bad second string"; break; }
            models[k].extra.assign((const char*)raw + p, l2);
            p += l2;
            if (!take(models[k].m, 0x40)) { bad = "truncated matrix"; break; }
        }
        if (!bad) recs->push_back(std::move(models));
    }
    if (!bad && p != rawLen) bad = "trailing bytes";
    free(raw);
    return bad;
}

// The replay: asset_inject_<letter>.txt ("rm <id,id,...|->" then the stroke's
// base64) fills an EMPTY script proposal with the stroke's groups and the local
// ids the mod matched for its removals. The file is deleted once read.
static bool InjectAssetsFromFile(uint64_t r8)
{
    if (!g_dataDir[0]) return false;
    ReadInstance();
    if (!g_instance[0]) return false;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sasset_inject_%s.txt", g_dataDir, g_instance);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return false;
    if (!TerrainCarrierEmpty(r8)) {
        Log("[asset-inject] inject file present, but this script proposal is not empty -- left alone\n");
        return false;
    }
    FILE* f = _fsopen(path, "rb", _SH_DENYNO);
    if (!f) { Log("[asset-inject] inject file present but not readable\n"); return false; }
    _fseeki64(f, 0, SEEK_END);
    const long long len = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    std::string text;
    if (len > 0) {   // no size cap: the file is ours, as big as the stroke
        text.resize((size_t)len);
        if (fread(&text[0], 1, (size_t)len, f) != (size_t)len) text.clear();
    }
    fclose(f);
    DeleteFileA(path);

    std::vector<int32_t> rm;
    std::vector<std::vector<AssetModelSrc>> recs;
    if (const char* bad = ParseAssetStroke(text, &rm, &recs)) {
        Log("[asset-inject] %s (%lld B) -- left alone\n", bad, len);
        return false;
    }

    // removals: the local groups the mod matched by position
    for (int32_t id : rm) {
        uint64_t e = 0, c = 0;
        memcpy(&e, (void*)(r8 + 0x1e8), 8);
        memcpy(&c, (void*)(r8 + 0x1f0), 8);
        if (e == c) {
            ((uint64_t (*)(uint64_t*, uint64_t, const int32_t*))(g_base + RVA_VEC_INT_GROW))((uint64_t*)(r8 + 0x1e0), e, &id);
        } else {
            memcpy((void*)e, &id, 4);
            e += 4;
            memcpy((void*)(r8 + 0x1e8), &e, 8);
        }
    }
    // additions: one asset-group record per shipped group, built exactly as
    // MakeBuildAssetsProposal builds it
    uint64_t models = 0;
    for (auto& rec : recs) {
        alignas(16) uint8_t ce[0x8e0];
        ((void* (*)(uint8_t*))(g_base + RVA_CE_CTOR))(ce);
        memcpy(ce + 0x20, &ASSET_GROUP_TYPE, 4);
        ce[0x20d] = 1;
        alignas(16) uint8_t r48[0x48] = {};
        const float f75 = 0.75f, f25 = 2.5f;
        memcpy(r48 + 0x38, &f75, 4);
        memcpy(r48 + 0x3c, &f25, 4);
        uint64_t e48 = 0;
        memcpy(&e48, ce + 0x558, 8);
        ((uint64_t (*)(uint64_t*, uint64_t, uint8_t*))(g_base + RVA_VEC_48_GROW))((uint64_t*)(ce + 0x550), e48, r48);

        const size_t nm = rec.size();
        uint8_t* src = (uint8_t*)calloc(nm, 0x80);
        std::vector<char*> heaps;
        bool ok = src != nullptr;
        for (size_t k = 0; ok && k < nm; k++) {
            uint8_t* tm = src + k * 0x80;
            char* h1 = PutReadOnlyStdString(tm, rec[k].model);
            char* h2 = PutReadOnlyStdString(tm + 0x20, rec[k].extra);
            if (h1 == (char*)-1 || h2 == (char*)-1) ok = false;
            if (h1 && h1 != (char*)-1) heaps.push_back(h1);
            if (h2 && h2 != (char*)-1) heaps.push_back(h2);
            memcpy(tm + 0x40, rec[k].m, 0x40);
        }
        if (ok) {
            ((void (*)(uint64_t*, uint8_t*, uint8_t*))(g_base + RVA_VEC_TM_ASSIGN))((uint64_t*)(ce + 0x470), src, src + nm * 0x80);
            uint64_t e = 0, c = 0;
            memcpy(&e, (void*)(r8 + 0x200), 8);
            memcpy(&c, (void*)(r8 + 0x208), 8);
            if (e == c) {
                ((uint64_t (*)(uint64_t*, uint64_t, uint8_t*))(g_base + RVA_VEC_CE_GROW))((uint64_t*)(r8 + 0x1f8), e, ce);
            } else {
                ((void* (*)(uint64_t, uint8_t*))(g_base + RVA_CE_COPY_AT))(e, ce);
                e += 0x8e0;
                memcpy((void*)(r8 + 0x200), &e, 8);
            }
            models += nm;
        }
        for (char* h : heaps) free(h);
        free(src);
        ((void (*)(uint8_t*))(g_base + RVA_CE_DTOR))(ce);
        if (!ok) { Log("[asset-inject] out of memory building a group -- the carrier is partial\n"); break; }
    }
    Log("[asset-inject] filled the script proposal: %zu group(s), %llu model(s), %zu removal(s)\n",
        recs.size(), (unsigned long long)models, rm.size());
    return true;
}
