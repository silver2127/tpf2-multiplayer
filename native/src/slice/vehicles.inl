// vehicles.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// vehicle configuration off a buy, VBUY, the ARMED and NATIVE notices


// VBUY: a player's BuyVehicle, shipped for replication. The config is decoded
// from the by-value TransportVehicleConfig on the caller's stack (st[0]):
// parts at +0x00 (0x80 stride: modelId +0x00, reversed +0x04, loadConfig +0x08,
// color +0x20, autoLoadConfig +0x60), vehicleGroups at +0x18 -- every offset a
// ground-truth EXACT match (docs/re/COMMANDS.md; reversed from the member
// offsets RegisterUsertypesVehicle hands the VehiclePart usertype). The depot
// travels as its entity id; the Lua side on THIS instance turns it into a
// position and the model ids into file names before anything crosses to the
// peer. Cancelled and replayed at the stamp while a session is live (see the
// cancel decision in DeferHandler).
//   VBUY <depot> <nParts> { <model> <rev> <nLoad> <load..> <r> <g> <b> <nAuto> <auto..> }* <nGroups> <group..>
// <rev> is the part's reversed flag (0/1): without it every replayed wagon
// faced forward, and a train bought with a turned car (an ICE's tail head, a
// cab car) came out wrong on every instance, the originator's included
// (tearded's fork, 2026-09-20).
//
// The config half is shared with VREPL (ReplaceVehicle takes the SAME
// TransportVehicleConfig), so validation and encoding live in these two helpers
// rather than being written twice: one builder on the Lua side parses both
// lines, so the two encoders drifting apart would be a silent wire break.
//
// VCfgParts validates the config and hands back the parts vector; it returns -1
// when the struct cannot be trusted, and NOTHING may be written in that case --
// a half-written line would corrupt every command after it in the inject file.
// The config is read WHOLE, at whatever size the game holds: a train of any
// length, a part's load config and autoload words of any count, any number of
// vehicle groups. It used to stop at 64 parts and 256 ints per vector, and a
// 65-wagon train came back "config not readable -- runs natively, not shipped":
// bought on one instance, on no other, a hard desync. Each vector is checked
// here, before anything is written, so a config that does not read refuses
// as a whole (nothing half-written) and names the vector and its size.
static int VCfgParts(uint64_t cfg, uint64_t* partsBase, const char* tag)
{
    if (!IsHeapPtr(cfg) || !Readable((void*)cfg, 0x30)) {
        Log("[slice] %s: config pointer unreadable -- not shipped\n", tag);
        return -1;
    }
    uint64_t ub = 0;
    uint64_t uspan = ReadVecAny(cfg + 0x00, &ub, tag);
    if (!uspan || uspan % 0x80 != 0) {
        Log("[slice] %s: parts span %llu not a multiple of 0x80 -- not shipped\n",
            tag, (unsigned long long)uspan);
        return -1;
    }
    const int units = (int)(uspan / 0x80);
    std::vector<int32_t> v;
    for (int k = 0; k < units; k++) {
        const uint64_t u = ub + (uint64_t)k * 0x80;
        if (!ReadIntVec(u + 0x08, &v, tag)) {
            Log("[slice] %s: part %d of %d: loadConfig vector unreadable -- not shipped\n", tag, k + 1, units);
            return -1;
        }
        if (!ReadIntVec(u + 0x60, &v, tag)) {
            Log("[slice] %s: part %d of %d: autoLoadConfig vector unreadable -- not shipped\n", tag, k + 1, units);
            return -1;
        }
    }
    if (!ReadIntVec(cfg + 0x18, &v, tag)) {
        Log("[slice] %s: vehicleGroups vector unreadable -- not shipped\n", tag);
        return -1;
    }
    *partsBase = ub;
    return units;
}

// Everything after the leading entity field: the part count, one record per
// part, then the vehicle groups. Returns the group count (for the log line).
// Every vector was validated by VCfgParts; it is re-read here at full length.
static int WriteVehicleConfig(FILE* f, uint64_t cfg, uint64_t ub, int units)
{
    fprintf(f, " %d", units);
    std::vector<int32_t> v;
    for (int k = 0; k < units; k++) {
        uint64_t u = ub + (uint64_t)k * 0x80;
        int32_t model = 0;
        memcpy(&model, (void*)(u + 0x00), 4);
        uint8_t rev = 0;
        memcpy(&rev, (void*)(u + 0x04), 1);      // bool reversed, right behind modelId
        fprintf(f, " %d %d", model, rev ? 1 : 0);
        ReadIntVec(u + 0x08, &v, "loadConfig");
        fprintf(f, " %d", (int)v.size());
        for (size_t j = 0; j < v.size(); j++) fprintf(f, " %d", v[j]);
        float c[3] = { -1, -1, -1 };
        memcpy(c, (void*)(u + 0x20), 12);
        fprintf(f, " %.4f %.4f %.4f", c[0], c[1], c[2]);
        ReadIntVec(u + 0x60, &v, "autoLoadConfig");
        fprintf(f, " %d", (int)v.size());
        for (size_t j = 0; j < v.size(); j++) fprintf(f, " %d", v[j]);
    }
    ReadIntVec(cfg + 0x18, &v, "vehicleGroups");
    const int ng = (int)v.size();
    fprintf(f, " %d", ng);
    for (size_t j = 0; j < v.size(); j++) fprintf(f, " %d", v[j]);
    return ng;
}

// Returns true when the VBUY line was written.
static bool WriteInjectVBuy(uint64_t depot, uint64_t cfg)
{
    uint64_t ub = 0;
    int units = VCfgParts(cfg, &ub, "VBUY");
    if (units < 0) return false;
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return false; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return false; }
    fprintf(f, "VBUY %d", (int)(int32_t)depot);
    int ng = WriteVehicleConfig(f, cfg, ub, units);
    fprintf(f, "\n");
    fclose(f);
    Log("[slice] VBUY shipped: depot=%d parts=%d groups=%d\n", (int)(int32_t)depot, units, ng);
    return true;
}

// Vehicle commands that REFERENCE vehicles ship raw local entity ids; the Lua
// side turns them into cross-peer keys (a purchase's origin:seq, or s:<id> for
// a save vehicle) and the peer maps them back. ARMED says whether the local
// command was cancelled.
//   VSELL  <n> <id..>            SellVehicle  (r8 = &vector<Entity>)
//   VDEPOT <vehicle> <sell01>    SendToDepot  (r8 = Entity, r9 = bool)
//   VSTOP  <vehicle> <stopped01> SetUserStopped (r8 = Entity, r9 = bool)
//   VLINE  <vehicle> <line> <stopIndex>   SetLine (r8, r9 = Entity, st[0] = int)
//   VREPL  <vehicle> <config..>  ReplaceVehicle (r8 = Entity, r9 = config*)
// VREPL's payload after the vehicle is byte-for-byte what VBUY writes after the
// depot -- the same TransportVehicleConfig, the same encoder -- so the Lua side
// builds the config for both lines with one function.
// Written just before a capture: was the local build CANCELLED (1), so the
// originator must replay it at the stamp, or left to run natively (0), so the
// originator must NOT replay it. The Lua used to infer this from its own
// peer-seen flag while the slice decided from the status file; the two could
// disagree for a few seconds after a join, and the originator then built the
// road natively AND replayed it (review, 2026-08-31). One decision, written
// down, read by both halves.
static void WriteArmed(bool armed)
{
    ReadInstance();
    if (!g_instance[0]) return;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) return;
    fprintf(f, "ARMED %d\n", armed ? 1 : 0);
    fclose(f);
}

// A player build left NATIVE in a LIVE session because its record did not decode,
// so it could not be cancelled. No capture line carries it, and the mod no longer
// scans the world on a timer, so this asks for one catch-up scan instead; without
// it the build would stand on this instance only.
static void WriteNativeNotice(const char* kind)
{
    ReadInstance();
    if (!g_instance[0]) return;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) return;
    fprintf(f, "NATIVE %s\n", kind);
    fclose(f);
}

