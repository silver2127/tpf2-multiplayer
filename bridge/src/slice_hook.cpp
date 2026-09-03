// ---------------------------------------------------------------------------
// slice_hook -- the vertical slice: capture a player's road, cancel it locally,
// and hand it to the lockstep engine to execute on every peer at an agreed
// stamp.
//
// This is the first hook that closes the loop. defer_hook proved a build can be
// CANCELLED; args_probe proved the geometry can be READ; lockstep.lua proved a
// command can be EXECUTED at an agreed game time on both peers. Each worked
// alone and none of them were connected.
//
// WHY TWO HOOKS AND NOT ONE
// Cancelling at StreetBuilder::UpdateEngine (what defer_hook does) is fatal
// here: UpdateEngine is what CALLS make_cmd::BuildProposal, so suppressing it
// means the proposal is never built and there is nothing to read. Capture and
// cancel have to straddle the proposal's construction:
//
//     StreetBuilder::UpdateEngine 0x459ce0
//         -> make_cmd::BuildProposal 0x9dc750   (returns to 0x459e97)  CAPTURE
//         -> CommandList::Add        0x9d2a00   (returns to 0x459eb7)  CANCEL
//
// The two calls are consecutive statements in one function, so the geometry is
// fully formed at the first and the command has not yet been queued at the
// second.
//
// WHY CANCELLING AT CommandList::Add IS SAFE
// Its return value is discarded at this call site. Disassembled:
//     0x459eb2:  e8 49 8b 57 00     call 0x9d2a00
//     0x459eb7:  48 8d 4c 24 38     lea  rcx,[rsp+0x38]
// The next instruction loads rcx; nothing reads rax. Same property that made
// suppression safe at UpdateEngine, established the same way -- by reading the
// call site rather than assuming.
//
// WHY THE CALLER RVA FILTER IS LOAD-BEARING, NOT A TIDINESS CHECK
// CommandList::Add has 82 call sites and runs ~100/sec from the Lua bridge.
// The lockstep mod's own replicated builds go through it too. Suppressing on
// anything but caller_rva == 0x459eb7 would cancel the replay of the very
// command this hook just captured, and the road would vanish on both peers
// while the logs claimed success.
//
// Suppression defaults OFF. With it off this is a pure observer: the player's
// road is built locally as usual AND written to the inject file, which desyncs
// on purpose -- that is the wiring test, not the feature.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <share.h>
#include "hook.h"
#include "datadir.h"

// ---------------------------------------------------------------------------
// GAME BUILD GUARD. Every RVA below was measured on ONE build of
// TransportFever2.exe. Patching those offsets into any other build writes a
// jmp into the middle of whatever instruction happens to live there, which
// is a crash at best and a silently corrupted command at worst. So the PE
// header of the running exe is compared against the build the RVAs belong
// to before a single byte is patched; on mismatch the DLL logs and stays
// inert. Both values come from the installed exe's IMAGE_NT_HEADERS
// (FileHeader.TimeDateStamp at nt+0x08, OptionalHeader.SizeOfImage at
// nt+0x50 for PE32+) and change on every rebuild of the game.
// ---------------------------------------------------------------------------
static const DWORD GAME_BUILD_NUMBER      = 35924;        // Transport Fever 2 build the RVAs were measured on
static const DWORD GAME_EXE_TIMEDATESTAMP = 0x675abcc6;   // IMAGE_FILE_HEADER.TimeDateStamp
static const DWORD GAME_EXE_SIZEOFIMAGE   = 0x046ce000;   // IMAGE_OPTIONAL_HEADER64.SizeOfImage

// BuildProposal: steal 19, the size args_probe already runs against this
// function. CommandList::Add: 8 pushes (13 bytes) + lea rbp,[rsp-0x78] (5) = 18,
// a clean boundary that stops short of the RIP-relative mov rax,[rip+..] at
// offset 34. The trampoline re-executes rsp-relative code, which is safe only
// because the relay restores rsp to its entry value first.
static const uintptr_t RVA_BUILDPROPOSAL = 0x9dc750;
static const int       STEAL_BUILDPROPOSAL = 19;
static const uintptr_t RVA_CMDADD = 0x9d2a00;
static const int       STEAL_CMDADD = 18;

// The two return addresses inside StreetBuilder::UpdateEngine.
static const uintptr_t CALLER_BUILDPROPOSAL = 0x459e97;
static const uintptr_t CALLER_CMDADD        = 0x459eb7;
// The street/track UPGRADE tool (construction_util_street_upgrade: change road
// or track type, add/remove catenary). It submits its own BuildProposal from a
// different return address than the builder, with the replace-in-place shape
// nodesToAdd=0, edgesToAdd=N, edgesToRemove=N -- every endpoint is an EXISTING
// node, so every id in the proposal is positive (proposal dumps 2026-08-30).
// Until now this landed in the "not the road path -- ignored" branch, so an
// upgrade applied locally and never replicated. Captured, cancelled and
// replayed exactly like the road path; the only differences are that the
// removed edges have to travel (an upgrade with no removal list would build a
// SECOND edge on top of the old one on the peer) and that there is nothing to
// log about new nodes, because there are none.
static const uintptr_t CALLER_UPGRADE       = 0x4790fc;
// UI::Bulldozer::Apply's BuildProposal return address (r4_recon_dem.md A1:
// call at 0x3eb222, return addr 0x3eb227). This caller is classified and
// LOGGED only -- never cancelled, never injected, in this commit.
static const uintptr_t CALLER_BULLDOZE      = 0x3eb227;
// The sol2 wrapper's factory call site (Lua path: api.cmd.make.buyVehicle).
// A BuyVehicle from HERE is our own replay on the peer: shipping it back
// would ping-pong purchases between the two instances forever. NOT 0x74fd88:
// that is the UI's buy (vehiclemanager.cpp, ACTION_MAP) -- filtering it
// suppressed the player's real purchase (measured 2026-08-28). The cee***
// block is the scripting layer (cf. cee710 = SetVehicleManualDeparture's
// wrapper, ced378 = buildProposal's).
static const uintptr_t CALLER_LUA_VEHICLE   = 0xceefae;

static const int ID_BUILDPROPOSAL = 0;
static const int ID_CMDADD        = 1;
static const int BLOB_SIZE = 48;

// Every other command factory, same hook shape. Steal sizes are the ones
// args_probe ran against these functions live. ids 2..9; 0 and 1 are above.
struct Factory { uintptr_t rva; int steal; int id; const char* name; const char* kind; };
static const Factory FACTORIES[] = {
    { 0x9dca00, 15, 2, "BuyVehicle",     "vehicle" },
    { 0x9de380, 20, 3, "SellVehicle",    "vehicle" },
    { 0x9dddb0, 15, 4, "ReplaceVehicle", "vehicle" },
    { 0x9de6f0, 20, 5, "SendToDepot",    "vehicle" },
    { 0x9dea10, 18, 6, "SetLine",        "line"    },
    { 0x9dcde0, 19, 7, "CreateLine",     "line"    },
    { 0x9df4e0, 19, 8, "UpdateLine",     "line"    },
    { 0x9dd190, 20, 9, "DeleteLine",     "line"    },
    { 0x9ddfe0, 20, 10, "Reverse",        "vehicle" },  // steal from COMMAND_MAP.md
    { 0x9de8a0, 20, 13, "SetColor",       "sync"    },  // r9 -> CVec3f*, 3 floats
    { 0x9deb70, 15, 14, "SetName",        "sync"    },  // r9 -> std::string*, MSVC SSO
};
static const int NUM_FACTORIES = (int)(sizeof(FACTORIES) / sizeof(FACTORIES[0]));

// The Command we intend to cancel, identified by ADDRESS. A factory returns
// its 0x38-byte Command through the hidden pointer in rcx and hands that same
// pointer back in rax; the UI passes rax straight to CommandList::Add as r8
// (disassembled at 0x459e97: mov r8,rax ... call Add). So Add.r8 == factory.rcx
// names exactly the command just built, for every factory, with no per-channel
// caller RVA -- and it cannot match any of the ~100/s unrelated Adds from the
// Lua bridge, which carry different pointers.
static volatile LONG64 g_pendingCmd = 0;
// Set when the pending cancel is a fire-and-forget command (vehicle/line):
// suppress it at Add even if its completion callback cannot be fired, because
// nothing waits on it. Roads/builds leave this 0 -- their tool genuinely hangs.
static volatile LONG g_pendingNoCb = 0;

extern "C" void DeferRelay();

// Runtime data directory (trailing backslash), resolved once at attach via
// datadir.h: TPF2MP_DATADIR, else %LOCALAPPDATA%\tpf2mp\data, else this DLL's
// directory. Every file this DLL reads or writes at run time -- the log, the
// identity file, the inject files -- lives here. Empty until Init fills it.
static char g_dataDir[MAX_PATH] = "";
// This DLL's own directory (trailing backslash). The installer drops
// tpf2_slice.cfg next to the DLL, so the cfg is looked up here FIRST and in
// the data dir second.
static char g_dllDir[MAX_PATH] = "";

static FILE* g_log = nullptr;
static uintptr_t g_base = 0;
static long g_captured = 0, g_suppressed = 0, g_addSeen = 0;
static char g_instance[8] = "";

// Set by the BuildProposal hook, consumed by the CommandList::Add hook. Only a
// capture that actually produced geometry may cancel anything: if the decode
// fails, the build must be left alone rather than silently thrown away.
static volatile LONG g_pendingCancel = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[1200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, g_log);
    fflush(g_log);
}

static bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// Two independent switches, re-read per event so either can be flipped while the
// game runs. `enabled=0` makes the hook completely inert -- no capture, no
// inject, no cancel. That case previously had no off switch: turning suppression
// off still left the hook injecting every road, so the build happened locally AND
// replicated, giving the player a duplicate. Getting back to normal single-player
// behaviour meant marking the inject file read-only, which is not a control a
// user should have to know about.
//
// Lookup order for tpf2_slice.cfg: the copy next to this DLL (where the
// installer puts it), then the data dir, then built-in defaults if neither
// exists. First file found wins. Lines whose first non-blank character is
// '#' or ';' are comments and can never flip a switch -- the matching is a
// substring test, so without this a commented-out "# suppress=1" would still
// cancel builds.
static FILE* OpenCfg()
{
    char p[MAX_PATH];
    if (g_dllDir[0]) {
        snprintf(p, sizeof(p), "%stpf2_slice.cfg", g_dllDir);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (f) return f;
    }
    if (g_dataDir[0]) {
        snprintf(p, sizeof(p), "%stpf2_slice.cfg", g_dataDir);
        return _fsopen(p, "r", _SH_DENYNO);
    }
    return nullptr;
}

static bool CfgLineIsComment(const char* line)
{
    while (*line == ' ' || *line == '\t') line++;
    return *line == '#' || *line == ';';
}

static void ReadCfg(bool* enabled, bool* suppress, bool* groundtruth)
{
    *enabled = true;      // default on; the DLL is only present if asked for
    *suppress = false;    // see the no-cfg case below: that means lockstep
    if (groundtruth) *groundtruth = false;
    FILE* f = OpenCfg();
    if (!f) {
        // NO CFG AT ALL. This fell through to suppress=0, which is observe
        // mode: the build happens locally AND replicates, so that peer
        // silently runs a different protocol from every other one. Measured
        // 2026-08-31 on a joiner with no cfg installed -- it applied its own
        // builds at click time while the host cancelled and replayed at the
        // stamp, and the two worlds could not stay together. A missing file is
        // not a request for a debugging mode: real multiplayer is the default,
        // and the log says so once.
        *suppress = true;
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("[slice] no tpf2_slice.cfg found -- defaulting to suppress=1 "
                "(real lockstep); put a cfg next to the DLL to change it\n");
        }
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (CfgLineIsComment(line)) continue;
        if (strstr(line, "enabled=0"))     *enabled = false;
        if (strstr(line, "suppress=1"))    *suppress = true;
        if (strstr(line, "suppress=0"))    *suppress = false;   // explicit observe mode
        if (groundtruth && strstr(line, "groundtruth=1")) *groundtruth = true;
    }
    fclose(f);
}

// Instance letter, so the road lands in this peer's inject file and not the
// other's. Line 1 of tpf2_instance.txt is the letter; line 2 is "pid=<n>".
//
// The pid line is load-bearing under Sandboxie: B reads its OVERLAY copy only
// while that copy exists -- delete it and the read silently falls through to
// the host's file, so B would impersonate A and append its builds to an inject
// file nothing reads (r7_analysis_lin.md F1, fall-through INFERRED from
// Sandboxie copy-on-write semantics). Binding the identity to
// GetCurrentProcessId turns that silent loss into a loud refusal: g_instance
// stays empty, the attach line prints instance=?, and WriteInject refuses
// ("no instance letter -- cannot inject").
// RE-READ, NEVER CACHED. The joiner's bridge picks its letter from which
// local port is free, so on a machine running one game it claims 'a' and
// writes that into tpf2_instance.txt. The LOBBY then hands it the joiner
// role and the bridge and Lua both become 'b' -- but a slice that read the
// letter once at attach keeps writing captures into lockstep_inject_a.txt,
// which nothing on that machine reads. Everything the joining player does
// is then dropped in silence: measured 2026-08-31, a vehicle purchase and
// every build on the joiner never reached the host, while the host's own
// commands replayed there perfectly (inject_a.txt fresh, inject_b.txt eight
// hours stale, Lua logging [ls-b] the whole time). The file is 22 bytes and
// a capture happens when a player clicks, so re-reading costs nothing.
static void ReadInstance()
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%stpf2_instance.txt", g_dataDir);
    FILE* f = _fsopen(p, "r", _SH_DENYNO);
    if (!f) return;
    if (fgets(g_instance, sizeof(g_instance), f)) {
        for (char* c = g_instance; *c; c++)
            if (*c == '\r' || *c == '\n' || *c == ' ') { *c = 0; break; }
    }
    char line2[64];
    unsigned long want = 0;
    unsigned long mine = GetCurrentProcessId();
    if (fgets(line2, sizeof(line2), f) && sscanf(line2, "pid=%lu", &want) == 1) {
        if (want != mine) {
            Log("[slice] identity file pid=%lu != mine %lu -- refusing instance "
                "letter '%s' (fell through to the other peer's file?)\n",
                want, mine, g_instance);
            g_instance[0] = 0;
        }
    } else {
        Log("[slice] identity file has no pid line -- refusing instance letter "
            "'%s' (mine pid=%lu)\n", g_instance, mine);
        g_instance[0] = 0;
    }
    fclose(f);
}
// ---------------------------------------------------------------------------
// IS A MULTIPLAYER SESSION ACTUALLY RUNNING?
//
// Cancelling a build is only safe because something replays it. Nothing else in
// this DLL checks that anything will: install the MSI, load a save with the mod
// switched off, and every build would be cancelled by a hook whose replay half
// is not there -- the player simply cannot build. A mod that breaks the base
// game when it is not in use is not acceptable, so the cancel is gated on
// evidence that the other half is alive.
//
// The evidence is already on disk: the Lua writes lockstep_status_<letter>.txt
// every tick, carrying its own game time and the peer's. Fresh file = the mod
// is running. A peer time in it = somebody is actually playing with us. Solo
// with the mod on is therefore ALSO native: nothing needs replaying, so nothing
// is cancelled, and the build behaves exactly as it does in a stock game.
//
// Cached for a second: this is asked once per player action, not per frame.
static bool SessionLive()
{
    static ULONGLONG lastCheck = 0;
    static bool cached = false;
    const ULONGLONG now = GetTickCount64();
    if (lastCheck && now - lastCheck < 1000) return cached;
    lastCheck = now;
    cached = false;

    ReadInstance();
    if (!g_instance[0]) return cached;   // no identity yet: nothing can replay

    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_status_%s.txt", g_dataDir, g_instance);

    // Freshness first: a stale file is a mod that is not running (or a save
    // loaded without it).
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) return cached;
    FILETIME ftNow; GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER a, b;
    a.LowPart = fa.ftLastWriteTime.dwLowDateTime; a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    b.LowPart = ftNow.dwLowDateTime;             b.HighPart = ftNow.dwHighDateTime;
    if (b.QuadPart < a.QuadPart) return cached;
    const ULONGLONG ageMs = (b.QuadPart - a.QuadPart) / 10000ULL;
    // The Lua writes this file every 15 ticks, about 2.8 s at the usual tick
    // rate -- so a 3 s freshness window was a coin flip, and losing it means a
    // build runs natively, un-replicated, with no error (review, 2026-08-31).
    // 15 s still notices a mod that is not running long before it matters.
    if (ageMs > 15000) return cached;

    // Then a peer: "t=1759  peer=1760  skew=-1.0 ...". No peer field, or the
    // Lua reporting none, means a solo game -- let the engine build natively.
    FILE* f = _fsopen(p, "r", _SH_DENYNO);
    if (!f) return cached;
    char line[256] = {0};
    if (fgets(line, sizeof(line), f)) {
        const char* pk = strstr(line, "peer=");
        if (pk) {
            double pt = 0.0;
            if (sscanf(pk + 5, "%lf", &pt) == 1 && pt > 0.0) cached = true;
        }
    }
    fclose(f);
    return cached;
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
struct Edge { int32_t node0, node1; float t0[3], t1[3]; int32_t btype, bidx; };

static int DecodeEdges(uint64_t a2, Edge* out, int maxOut)
{
    if (!Readable((void*)(a2 + 0x18), 16)) return 0;
    uint64_t begin = 0, end = 0;
    memcpy(&begin, (void*)(a2 + 0x18), 8);
    memcpy(&end, (void*)(a2 + 0x20), 8);
    if (begin < 0x10000 || end <= begin) return 0;
    uint64_t span = end - begin;
    if (span % 120 != 0 || span > 0x20000) return 0;
    int n = (int)(span / 120);
    if (n > maxOut) n = maxOut;
    if (!Readable((void*)begin, (size_t)span)) return 0;
    const uint8_t* b = (const uint8_t*)begin;
    for (int i = 0; i < n; i++) {
        memcpy(&out[i].node0, b + i * 120 + 0x08, 4);
        memcpy(&out[i].node1, b + i * 120 + 0x0c, 4);
        memcpy(out[i].t0,     b + i * 120 + 0x10, 12);
        memcpy(out[i].t1,     b + i * 120 + 0x1c, 12);
        memcpy(&out[i].btype, b + i * 120 + 0x28, 4);
        memcpy(&out[i].bidx,  b + i * 120 + 0x2c, 4);
    }
    return n;
}

// Removal lists.
//
// A road joining an existing one MID-SPAN splits it: the proposal inserts a new
// node and adds both halves as new edges, which is only valid if the ORIGINAL
// edge is also removed. Capture "ROADE 2 ... 5" showed exactly that shape --
// two new nodes producing five edges, four of them referencing real entities --
// and it failed on replay because edgesToRemove was never carried.
//
// The add-vectors sit at a2+0x00 (nodes) and a2+0x18 (edges); a2+0x30 and
// a2+0x48 are the next two triplets and read as all-zero in every free-standing
// capture, which is what an empty removal list looks like. That is a hypothesis,
// so the counts are LOGGED: a build that splits two existing edges should report
// exactly two removals. If the numbers do not match the topology, the offsets
// are wrong and nothing downstream should trust them.
static int DecodeIds(uint64_t vec, int32_t* out, int maxOut)
{
    if (!Readable((void*)vec, 16)) return 0;
    uint64_t begin = 0, end = 0;
    memcpy(&begin, (void*)vec, 8);
    memcpy(&end, (void*)(vec + 8), 8);
    if (begin < 0x10000 || end <= begin) return 0;
    uint64_t span = end - begin;
    if (span % 4 != 0 || span > 0x10000) return 0;
    int n = (int)(span / 4);
    if (n > maxOut) n = maxOut;
    if (!Readable((void*)begin, (size_t)span)) return 0;
    memcpy(out, (void*)begin, (size_t)n * 4);
    return n;
}

// Find the removal vectors by SHAPE instead of guessing an offset.
//
// a2+0x30 and a2+0x48 were the obvious candidates -- the next two triplets after
// nodesToAdd and edgesToAdd, all-zero in every free-standing capture. Wrong: a
// 3-node road reported 30 entries there. An empty vector reads as zero whatever
// it actually is, so "zero when I expect empty" was never evidence.
//
// Scan the whole struct for int32 vectors and log every candidate with its
// contents. A junction that splits ONE road must remove exactly ONE edge, so the
// right vector is identifiable by its count matching the topology -- which is a
// test the data has to pass, not an offset I picked.
static void DumpIdVectors(uint64_t a2)
{
    for (unsigned off = 0; off + 16 <= 0x120; off += 8) {
        if (off == 0x00 || off == 0x18) continue;     // known: nodes, edges
        if (!Readable((void*)(a2 + off), 16)) continue;
        uint64_t begin = 0, end = 0;
        memcpy(&begin, (void*)(a2 + off), 8);
        memcpy(&end, (void*)(a2 + off + 8), 8);
        if (begin < 0x10000 || end <= begin) continue;
        uint64_t span = end - begin;
        if (span % 4 != 0 || span > 0x400) continue;
        int n = (int)(span / 4);
        if (!Readable((void*)begin, (size_t)span)) continue;
        const int32_t* v = (const int32_t*)begin;
        char line[600];
        int o = snprintf(line, sizeof(line), "[slice]   IDVEC +%03x n=%d:", off, n);
        for (int i = 0; i < n && i < 24 && o < (int)sizeof(line) - 16; i++)
            o += snprintf(line + o, sizeof(line) - o, " %d", (int)v[i]);
        Log("%s\n", line);
    }
}

static int DecodeNodes(uint64_t a2, Node* out, int maxOut)
{
    if (!Readable((void*)a2, 16)) return 0;
    uint64_t begin = 0, end = 0;
    memcpy(&begin, (void*)a2, 8);
    memcpy(&end, (void*)(a2 + 8), 8);
    if (begin < 0x10000 || end <= begin) return 0;
    uint64_t span = end - begin;
    if (span % 24 != 0 || span > 0x20000) return 0;
    int n = (int)(span / 24);
    if (n > maxOut) n = maxOut;
    if (!Readable((void*)begin, (size_t)span)) return 0;
    const uint8_t* b = (const uint8_t*)begin;
    for (int i = 0; i < n; i++) {
        memcpy(&out[i].x,  b + i * 24 + 0x00, 4);
        memcpy(&out[i].y,  b + i * 24 + 0x04, 4);
        memcpy(&out[i].z,  b + i * 24 + 0x08, 4);
        // The placeholder id (-1, -2, ...). Edges address nodes by THIS, not by
        // position in the vector, so it has to travel with the geometry.
        memcpy(&out[i].id, b + i * 24 + 0x14, 4);
    }
    return n;
}

// Dump the first edge record so the street/track TYPE can be located.
//
// execPolyline currently hardcodes street type 16 and edge type 0, which means
// every replicated edge comes out as one fixed road type and a railway
// replicates as a road. The type must be somewhere in this 120-byte record --
// the decode so far covers only id, node0, node1 and the two tangents.
//
// Logging the whole record on every capture turns the fix into a diff across
// builds the player was going to make anyway: one road of type A, one of type B,
// one railway. Whatever changes is the type field. That is the same technique
// that decoded the node record, and it beats another dedicated probe cycle.
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
    if (t.type == 0) {
        t.trackType = 1;                               // not applicable on a street
        if (t.streetType < 0 || t.streetType > 512) return t;
    } else {
        if (t.trackType < 0 || t.trackType > 512) return t;
    }
    t.ok = true;
    return t;
}

static void DumpFirstEdge(uint64_t a2)
{
    uint64_t begin = 0, end = 0;
    if (!Readable((void*)(a2 + 0x18), 16)) return;
    memcpy(&begin, (void*)(a2 + 0x18), 8);
    memcpy(&end, (void*)(a2 + 0x20), 8);
    if (begin < 0x10000 || end <= begin) return;
    uint64_t span = end - begin;
    if (span % 120 != 0 || span > 0x20000) {
        Log("[slice]   edge vector span=%llu (not a multiple of 120) -- layout differs\n",
            (unsigned long long)span);
        return;
    }
    if (!Readable((void*)begin, 120)) return;
    const uint8_t* b = (const uint8_t*)begin;
    Log("[slice]   edges=%llu, first record:\n", (unsigned long long)(span / 120));
    char line[160];
    for (int row = 0; row < 120 / 16 + 1 && row * 16 < 120; row++) {
        int o = snprintf(line, sizeof(line), "[slice]     +%02x: ", row * 16);
        for (int c = 0; c < 16 && row * 16 + c < 120; c++)
            o += snprintf(line + o, sizeof(line) - o, "%02x", b[row * 16 + c]);
        line[o] = 0;
        Log("%s\n", line);
    }
    // Every int32 that is small enough to be a resource index. The type is far
    // more likely to be one of these than a float or a pointer.
    for (int k = 0; k + 4 <= 120; k += 4) {
        int32_t v; memcpy(&v, b + k, 4);
        if (v > -4 && v < 256) Log("[slice]     I+%02x = %d\n", k, (int)v);
    }
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
static long g_conroad = 0;
static void WriteInjectConRoad(const Node* nodes, int n, const Edge* edges, int m,
                               const Edge* rme, int re, const EdgeType& et)
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
    fprintf(f, "\n");
    fclose(f);
}

// ---------------------------------------------------------------------------
// GROUND TRUTH
//
// Decoding by watching a player build things means one sample per five-minute
// cycle -- rebuild, restart, ask a human to draw a road -- which is why four
// separate field guesses this session rested on a single observation and all
// four were wrong.
//
// Invert it: drive KNOWN values in and see where they land. `api.cmd.make.
// buildProposal` from Lua reaches this exact factory (observed as
// caller_rva=ced378), and calling the factory WITHOUT sendCommand builds the
// Command and fires this hook while never touching the world. So a sweep of
// hundreds of parameter values costs a second, is non-destructive, and needs no
// restart and no human.
//
// Samples are self-identifying: the Lua side writes a sentinel into node 0's X
// as 900000 + testId*1000 + sampleIndex, so a capture carries its own label and
// nothing has to be matched up by ordering.
// Hex-dump a memory range in 64-byte chunks under one record tag. Chunked
// because Log() has a fixed buffer; the correlator reassembles by offset.
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

// A ground-truth sample is labelled in-band by a sentinel coordinate
// 900000 + testId*1000 + sampleIndex. STREET sweeps put it in node 0's X;
// CONSTRUCTION sweeps have no street nodes at all, so they put it in the
// construction's placement X, which lives at a3+0x368 (varied with position
// across five earlier captures -- a2 == a3+0x70 in every one of them).
static bool GtSentinel(double x, int* testId, int* sample)
{
    if (x < 900000.0 || x > 999999.0) return false;
    long s = (long)(x + 0.5);
    *testId = (s - 900000) / 1000;
    *sample = (s - 900000) % 1000;
    return true;
}

// ---------------------------------------------------------------------------
// Pointer chase for strings. The params.modules map of a construction is a
// native map<int, ModuleInfo> that is NOT in the raw proposal bytes -- the M8
// probe reached 'platform_cargo_era_a.module' at ProposalData +0xe8+0x40+0x38,
// three hops down. So: breadth-first over every qword that looks like a heap
// pointer, up to three levels, reporting any printable run that contains one
// of the needles together with the offset path that reached it. The path IS
// the layout: two samples that reach 'GTSENT_0.lua' and 'GTSENT_1.lua' by the
// same path have located the ModuleInfo record.
static bool IsHeapPtr(uint64_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFFULL; }

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
                if (strstr(tmp, ".module") || strstr(tmp, "station/") || strstr(tmp, "GTSENT") ||
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

// Where did the sentinel land? A script-fed proposal was never captured
// before, so +0x368 is an assumption for it. Scan every float in the struct.
static bool FindSentinel(uint64_t a3, unsigned len, int* testId, int* sample, unsigned* atOff)
{
    if (!Readable((void*)a3, len)) return false;
    const uint8_t* b = (const uint8_t*)a3;
    for (unsigned off = 0; off + 4 <= len; off += 4) {
        float f; memcpy(&f, b + off, 4);
        if (f == f && GtSentinel(f, testId, sample)) { *atOff = off; return true; }
    }
    return false;
}
static bool GroundTruthSample(uint64_t a2, uint64_t a3)
{
    int testId = 0, sample = 0;
    Node nodes[8];
    int n = DecodeNodes(a2, nodes, 8);

    if (n >= 1 && GtSentinel(nodes[0].x, &testId, &sample)) {
        // ---- street sweep: the first edge record is the whole payload ----
        Edge edges[8];
        int m = DecodeEdges(a2, edges, 8);
        Log("[gt] test=%d sample=%d nodes=%d edges=%d\n", testId, sample, n, m);
        uint64_t begin = 0, end = 0;
        if (Readable((void*)(a2 + 0x18), 16)) {
            memcpy(&begin, (void*)(a2 + 0x18), 8);
            memcpy(&end, (void*)(a2 + 0x20), 8);
            if (begin > 0x10000 && end > begin && Readable((void*)begin, 120)) {
                const uint8_t* b = (const uint8_t*)begin;
                char line[400];
                int o = snprintf(line, sizeof(line), "[gt] e%d.%d:", testId, sample);
                for (int i = 0; i < 120; i++)
                    o += snprintf(line + o, sizeof(line) - o, "%02x", b[i]);
                Log("%s\n", line);
            }
        }
        // Removal-vector rows for the demolish sweep (tests 4-6). Proposal base
        // = a2 (the StreetProposal is the FIRST member of
        // construction_builder_util::Proposal, r9_analysis_dem.md 2):
        //   a2+0x30  removedNodes,    24-B records  (r9 1, DECOMPILED
        //            MakeProposalRemove / CreateProposalReplace)
        //   a2+0x48  removedSegments, 120-B records (r9 1, DECOMPILED
        //            StreetProposal_RemoveSegment)
        //   a2+0x1e0 toRemove vector<Entity>        (r9 2, decompile only --
        //            UNVERIFIED; these dumps are the sweep input that settles it)
        {
            uint64_t rb30 = 0, rb48 = 0, rb1e0 = 0;
            uint64_t s30  = ReadVec(a2 + 0x30, &rb30, 0x2000);
            uint64_t s48  = ReadVec(a2 + 0x48, &rb48, 0x2000);
            uint64_t s1e0 = ReadVec(a2 + 0x1e0, &rb1e0, 0x1000);
            Log("[gt] spans%d.%d span30=%llu span48=%llu span1e0=%llu\n",
                testId, sample, (unsigned long long)s30,
                (unsigned long long)s48, (unsigned long long)s1e0);
            if (s30)
                GtDumpRange("r30_", testId, sample, (const uint8_t*)rb30,
                            (unsigned)(s30 > 240 ? 240 : s30), 0);
            if (s48)
                GtDumpRange("r48_", testId, sample, (const uint8_t*)rb48,
                            (unsigned)(s48 > 360 ? 360 : s48), 0);
            if (s1e0)
                GtDumpRange("r1e0_", testId, sample, (const uint8_t*)rb1e0,
                            (unsigned)(s1e0 > 64 ? 64 : s1e0), 0);
        }
        return true;
    }

    // ---- construction sweep: find the sentinel wherever it landed ----
    unsigned atOff = 0;
    if (!FindSentinel(a3, 0x440, &testId, &sample, &atOff)) return false;
    Log("[gt] test=%d sample=%d CONSTRUCTION nodes=%d sentinel at a3+%03x\n",
        testId, sample, n, atOff);
    ChaseStrings(testId, sample, a3, 0x440);

    // The construction half of the proposal, raw. 0x1c0..0x420 covers the
    // 23-int vector, the 24 param-key strings, the 2272-byte record, and the
    // position, with margin either side. Nothing here is interpreted -- the
    // correlator decides what moved.
    const unsigned LO = 0x1c0, HI = 0x420;
    if (Readable((void*)(a3 + LO), HI - LO))
        GtDumpRange("c", testId, sample, (const uint8_t*)(a3 + LO), HI - LO, LO);

    // Every std::vector reachable from that range, with its contents, so a
    // value that lives behind a pointer (a param VALUE next to its KEY, say) is
    // in the record too. Same shape test as the live probe: ascending
    // begin/end, 4-byte multiple, bounded.
    for (unsigned off = LO; off + 16 <= HI; off += 8) {
        uint64_t b0 = 0, e0 = 0;
        memcpy(&b0, (void*)(a3 + off), 8);
        memcpy(&e0, (void*)(a3 + off + 8), 8);
        if (b0 < 0x10000 || e0 <= b0) continue;
        uint64_t span = e0 - b0;
        if (span > 0x1000 || (span & 3)) continue;
        if (!Readable((void*)b0, (size_t)span)) continue;
        char tag[16]; snprintf(tag, sizeof(tag), "v%03x_", off);
        GtDumpRange(tag, testId, sample, (const uint8_t*)b0, (unsigned)span, 0);
    }
    return true;
}

static bool CfgHas(const char* key)
{
    FILE* f = OpenCfg();
    // No cfg = the shipped defaults. ReadCfg already treats a missing file as
    // suppress=1; the switches here must agree, or a joiner with no cfg gets
    // suppress=1 but cancel_vehicle=0 -- its Reverse runs natively AND the Lua,
    // which hard-codes VREV as strict, replays it on top: a toggle applied twice
    // (review, 2026-08-31).
    if (!f) return strcmp(key, "cancel_vehicle") == 0 || strcmp(key, "merge") == 0;
    char line[128], want[64]; bool on = false;
    snprintf(want, sizeof(want), "%s=1", key);
    while (fgets(line, sizeof(line), f)) {
        if (CfgLineIsComment(line)) continue;
        if (strstr(line, want)) on = true;
    }
    fclose(f);
    return on;
}

static void GtDumpArg(int fid, const char* nm, uint64_t p, int testId, int sample)
{
    if (!IsHeapPtr(p) || !Readable((void*)p, 0x140)) return;
    char tag[24]; snprintf(tag, sizeof(tag), "f%d%s_", fid, nm);
    GtDumpRange(tag, testId, sample, (const uint8_t*)p, 0x140, 0);
    ChaseStrings(testId, sample, p, 0x140);
}

// TransportVehicleConfig ground-truth dump, for BuyVehicle (id 2, cfg = st[0]
// = [calleeRsp+0x28] -- r6_analysis_veh.md A, DECOMPILED buyVehicle_factory.c
// and MEASURED live by buyhook.cpp) and ReplaceVehicle (id 4, cfg = r9 --
// INFERRED from the funcsig text only, r2_recon_veh.md; sweep t40 verifies).
// Offsets relied on:
//   cfg+0x00  vector<TransportVehiclePart>, stride 0x80   (r6 B/C, DECOMPILED
//             vec_TransportVehiclePart_copy + TransportVehiclePart_copy_elem;
//             stride independently confirmed by the VehicleManager single-unit
//             replace colour copy, r6 F)
//   cfg+0x18  vector<int> vehicleGroups                   (r6 B, DECOMPILED)
//   unit+0x08 vector<int> loadConfig                      (r6 C, DECOMPILED)
//   unit+0x60 vector<int> autoLoadConfig                  (r6 C, DECOMPILED
//             copier; whether the STORAGE really is vector<int> is what sweep
//             t34 settles)
static void GtDumpVehicleConfig(int fid, uint64_t cfg, int testId, int sample)
{
    if (!IsHeapPtr(cfg) || !Readable((void*)cfg, 0x30)) return;
    char tag[24];
    snprintf(tag, sizeof(tag), "f%dcfg_", fid);
    GtDumpRange(tag, testId, sample, (const uint8_t*)cfg, 0x30, 0);

    uint64_t ub = 0;
    uint64_t uspan = ReadVec(cfg + 0x00, &ub, 0x80 * 16);
    Log("[gt] f%dunits%d.%d span=%llu\n", fid, testId, sample,
        (unsigned long long)uspan);
    if (uspan && uspan % 0x80 == 0) {
        int units = (int)(uspan / 0x80);
        for (int k = 0; k < units && k < 8; k++) {
            uint64_t u = ub + (uint64_t)k * 0x80;
            snprintf(tag, sizeof(tag), "f%du%d_", fid, k);
            GtDumpRange(tag, testId, sample, (const uint8_t*)u, 0x80, 0);
            uint64_t lb = 0, ab = 0;
            uint64_t lspan = ReadVec(u + 0x08, &lb, 0x100);
            if (lspan) {
                snprintf(tag, sizeof(tag), "f%dlc%d_", fid, k);
                GtDumpRange(tag, testId, sample, (const uint8_t*)lb, (unsigned)lspan, 0);
            }
            uint64_t aspan = ReadVec(u + 0x60, &ab, 0x100);
            if (aspan) {
                snprintf(tag, sizeof(tag), "f%dal%d_", fid, k);
                GtDumpRange(tag, testId, sample, (const uint8_t*)ab, (unsigned)aspan, 0);
            }
        }
    } else if (uspan) {
        // Stride 0x80 does not divide the span: dump the whole span in one
        // record and let the correlator find the real stride, rather than
        // trusting a bad guess.
        snprintf(tag, sizeof(tag), "f%duv_", fid);
        GtDumpRange(tag, testId, sample, (const uint8_t*)ub,
                    (unsigned)(uspan > 0x400 ? 0x400 : uspan), 0);
    }
    uint64_t gb = 0;
    uint64_t gspan = ReadVec(cfg + 0x18, &gb, 0x100);
    if (gspan) {
        snprintf(tag, sizeof(tag), "f%dgrp_", fid);
        GtDumpRange(tag, testId, sample, (const uint8_t*)gb, (unsigned)gspan, 0);
    }
}

// ecs::component::Line ground-truth dump, for CreateLine (id 7, line = st[0])
// and UpdateLine (id 8, line = r9) -- register maps DECOMPILED in
// r8_analysis_lin.md A. Offsets relied on:
//   line+0x00 vector<Line::Stop>, stride 0xa8  (r8 B/C, DECOMPILED: division
//             by 0xa8 in LineSystem_EntityAdded, +=0xa8 loops in AddStop)
//   line+0x18 float waitingTime, +0x1c LineVehicleInfo, size 0x24  (r8 B)
//   stop+0x10 vector<StationTerminal> alternativeTerminals  (r8 C -- INFERRED
//             by elimination; sweep T7 settles it)
//   stop+0x38 vector<SignalId> waypoints  (r8 C, DECOMPILED)
// The stops span is dumped WHOLE, not field-by-field: +0x04/+0x08 station vs
// terminal order inside a Stop is exactly what sweeps T3/T4 must decide (r8
// RISKS), so the correlator gets raw bytes rather than a pre-chewed guess.
static void GtDumpLine(int fid, uint64_t line, int testId, int sample)
{
    if (!IsHeapPtr(line) || !Readable((void*)line, 0x24)) return;
    char tag[24];
    snprintf(tag, sizeof(tag), "f%dline_", fid);
    GtDumpRange(tag, testId, sample, (const uint8_t*)line, 0x24, 0);

    uint64_t sb = 0;
    uint64_t sspan = ReadVec(line + 0x00, &sb, 0xa8 * 16);
    Log("[gt] f%dstopspan%d.%d span=%llu\n", fid, testId, sample,
        (unsigned long long)sspan);
    if (!sspan) return;
    snprintf(tag, sizeof(tag), "f%dstops_", fid);
    GtDumpRange(tag, testId, sample, (const uint8_t*)sb,
                (unsigned)(sspan > 0x540 ? 0x540 : sspan), 0);
    if (sspan >= 0xa8) {
        uint64_t ab = 0, wb = 0;
        uint64_t aspan = ReadVec(sb + 0x10, &ab, 0x100);
        if (aspan) {
            snprintf(tag, sizeof(tag), "f%ds0alt_", fid);
            GtDumpRange(tag, testId, sample, (const uint8_t*)ab, (unsigned)aspan, 0);
        }
        uint64_t wspan = ReadVec(sb + 0x38, &wb, 0x100);
        if (wspan) {
            snprintf(tag, sizeof(tag), "f%ds0wp_", fid);
            GtDumpRange(tag, testId, sample, (const uint8_t*)wb, (unsigned)wspan, 0);
        }
    }
}

// Targeted per-factory ground-truth dumps, run after the generic GtDumpArg
// pass once a sample has identified itself via its sentinel.
static void GtFactoryDumps(const Factory& f, uint64_t rdx, uint64_t r8, uint64_t r9,
                           const uint64_t* st, int testId, int sample)
{
    switch (f.id) {
    case 2:
        // BuyVehicle: r8=player, r9=depot (DECOMPILED asserts, r6 A), config =
        // first stack qword st[0] (r6 A, DECOMPILED + MEASURED by buyhook.cpp).
        GtDumpVehicleConfig(2, st[0], testId, sample);
        break;
    case 3: {
        // SellVehicle: r8 = const vector<Entity>* (r6 E, DECOMPILED assert
        // "!cmd.vehicleEntity.empty()"); dump the vector contents.
        uint64_t vb = 0;
        uint64_t vspan = IsHeapPtr(r8) ? ReadVec(r8, &vb, 0x400) : 0;
        Log("[gt] f3vecspan%d.%d span=%llu\n", testId, sample,
            (unsigned long long)vspan);
        if (vspan)
            GtDumpRange("f3vec_", testId, sample, (const uint8_t*)vb, (unsigned)vspan, 0);
        break;
    }
    case 4:
        // ReplaceVehicle: r8 = vehicleEntity; r9 = TransportVehicleConfig* is
        // INFERRED from the funcsig text (r2_recon_veh.md) -- not decompiled
        // for Replace; sweep t40 is what verifies it. st[0] is the bool slot.
        Log("[gt] f4 note: cfg-at-r9 is INFERRED (r2 funcsig, unverified) "
            "st0=%llx\n", (unsigned long long)st[0]);
        GtDumpVehicleConfig(4, r9, testId, sample);
        break;
    case 5:
        // SendToDepot: r8 = vehicleEntity, r9 = bool sellOnArrival -- INFERRED
        // from the funcsig signature order only (r6 G); sweep t39 confirms.
        Log("[gt] f5depot r8=%lld r9and1=%lld r9raw=%llx (register map INFERRED, "
            "r6 G)\n", (long long)r8, (long long)(r9 & 1),
            (unsigned long long)r9);
        break;
    case 6:
        // SetLine: r8=vehicle, r9=line, st[0]=stopIndex (r8_analysis_lin.md A,
        // from __FUNCSIG__ + the by-value Entity rule; not decompiled).
        Log("[gt] f6setline veh=%lld line=%lld stop=%lld\n",
            (long long)r8, (long long)r9, (long long)st[0]);
        break;
    case 7: {
        // CreateLine: rdx = std::string* name (MSVC SSO: len@+0x10, cap@+0x18,
        // chars inline iff cap<16 else heap ptr@+0x00 -- r8 A, DECOMPILED
        // make_cmd_CreateLine.sig.c free path), r8 = CVec3f* colour, r9 =
        // Entity player BY VALUE, st[0] = &component::Line (r8 A / r3 2b).
        if (IsHeapPtr(rdx) && Readable((void*)rdx, 0x20)) {
            uint64_t len = 0, cap = 0;
            memcpy(&len, (void*)(rdx + 0x10), 8);
            memcpy(&cap, (void*)(rdx + 0x18), 8);
            const char* chars = nullptr;
            if (cap < 16) chars = (const char*)rdx;
            else {
                uint64_t hp = 0;
                memcpy(&hp, (void*)rdx, 8);
                if (IsHeapPtr(hp) &&
                    Readable((void*)hp, (size_t)(len < 256 ? len : 256)))
                    chars = (const char*)hp;
            }
            char nm[64] = "";
            if (chars) {
                unsigned take = (unsigned)(len < 63 ? len : 63);
                memcpy(nm, chars, take);
                nm[take] = 0;
            }
            Log("[gt] f7name len=%llu cap=%llu heap=%d \"%s\"\n",
                (unsigned long long)len, (unsigned long long)cap,
                cap >= 16 ? 1 : 0, nm);
        }
        if (IsHeapPtr(r8) && Readable((void*)r8, 12)) {
            float c[3];
            memcpy(c, (void*)r8, 12);
            Log("[gt] f7color %.4f %.4f %.4f\n", c[0], c[1], c[2]);
        }
        Log("[gt] f7player %lld\n", (long long)r9);
        GtDumpLine(7, st[0], testId, sample);
        break;
    }
    case 8:
        // UpdateLine: r9 = &component::Line, a by-value struct passed as a
        // pointer to the caller temp (r8 A, DECOMPILED). Read at entry ONLY:
        // the factory moves the stops vector out and destroys the temp
        // (r8 A NOTE) -- this steal-prologue hook runs at entry, so it is safe.
        GtDumpLine(8, r9, testId, sample);
        break;
    case 9:
        // DeleteLine: r8 = Entity line by value (r8 A, funcsig).
        Log("[gt] f9delline line=%lld\n", (long long)r8);
        break;
    }
}

// Bulldozer classification -- LOG ONLY: this path never cancels and never
// injects (a cancelled-but-undecodable demolish would silently destroy the
// player's action; "never cancel on a failed decode" applies doubly here).
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
static bool LogBulldoze(uint64_t r8)
{
    bool shipped = false;
    __try {
        uint64_t nb = 0, eb = 0, tb = 0;
        uint64_t nspan = ReadVec(r8 + 0x30, &nb, 0x20000);
        uint64_t espan = ReadVec(r8 + 0x48, &eb, 0x20000);
        uint64_t tspan = ReadVec(r8 + 0x1e0, &tb, 0x10000);
        int rn = (int)(nspan / 24), re = (int)(espan / 120), nrem = (int)(tspan / 4);
        int nadd = 0;
        if (Readable((void*)(r8 + 0x1f8), 16)) {
            uint64_t ab = 0, ae = 0;
            memcpy(&ab, (void*)(r8 + 0x1f8), 8);
            memcpy(&ae, (void*)(r8 + 0x200), 8);
            if (ae > ab) nadd = (int)((ae - ab) / 0x8e0);
        }
        Log("[slice] BULLDOZE rn=%d re=%d toRemove=%d toAdd=%d "
            "(toRemove/toAdd offsets UNVERIFIED -- decompile only; log only, "
            "never cancelled)\n", rn, re, nrem, nadd);
        if (nspan % 24)
            Log("[slice]   removedNodes span=%llu not a multiple of 24\n",
                (unsigned long long)nspan);
        if (espan % 120)
            Log("[slice]   removedSegments span=%llu not a multiple of 120\n",
                (unsigned long long)espan);
        if (nrem >= 1 && nadd >= 1)
            Log("[slice]   UPGRADE-shaped (toRemove+toAdd) -- module edit, left "
                "to the con poll, not a demolish\n");
        else if (nrem >= 1)
            Log("[slice]   construction-demolish shape\n");
        else if (re >= 1 || rn >= 1) {
            Log("[slice]   edge-demolish shape\n");
            // Gated OFF by default. A removal is not self-correcting the way an
            // addition is: a road that fails to appear is a visible missing
            // road, but a road removed on the wrong instance is destroyed work
            // with nothing to rebuild it from. It ships only once the player
            // has opted in with road_demolish=1.
            if (CfgHas("road_demolish"))
                shipped = WriteBulldozeInject(nb, rn, eb, re);
            else
                Log("[slice]   (road_demolish not set -- NOT shipped; peers keep "
                    "this road and will diverge)\n");
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
        if (tspan >= 4) {
            int32_t c0;
            memcpy(&c0, (void*)tb, 4);
            Log("[slice]   toRemove[0]=%d (offset +0x1e0 UNVERIFIED)\n", c0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[slice] BULLDOZE classification faulted -- ignored, build proceeds\n");
        return false;   // never cancel on a failed decode
    }
    return shipped;
}

// Generic capture for the vehicle and line factories. Observe-only unless the
// channel cancel switch is on; nothing is injected yet because nothing is
// decoded yet -- the point of this is the ground-truth sweep that DOES the
// decoding. An Entity argument is a plain integer, so a Lua-issued command with
// a sentinel entity (900000 + test*1000 + i) labels its own sample.
// VBUY: a player's BuyVehicle, shipped for replication. The config is decoded
// from the by-value TransportVehicleConfig on the caller's stack (st[0]):
// parts at +0x00 (0x80 stride: modelId +0x00, loadConfig +0x08, color +0x20,
// autoLoadConfig +0x60), vehicleGroups at +0x18 -- every offset a ground-truth
// EXACT match (COMMAND_ARGS.md). The depot travels as its entity id; the Lua
// side on THIS instance turns it into a position and the model ids into file
// names before anything crosses to the peer. Never cancelled.
//   VBUY <depot> <nParts> { <model> <nLoad> <load..> <r> <g> <b> <nAuto> <auto..> }* <nGroups> <group..>
//
// The config half is shared with VREPL (ReplaceVehicle takes the SAME
// TransportVehicleConfig), so validation and encoding live in these two helpers
// rather than being written twice: one builder on the Lua side parses both
// lines, so the two encoders drifting apart would be a silent wire break.
//
// VCfgParts validates the config and hands back the parts vector; it returns -1
// when the struct cannot be trusted, and NOTHING may be written in that case --
// a half-written line would corrupt every command after it in the inject file.
static int VCfgParts(uint64_t cfg, uint64_t* partsBase, const char* tag)
{
    if (!IsHeapPtr(cfg) || !Readable((void*)cfg, 0x30)) {
        Log("[slice] %s: config pointer unreadable -- not shipped\n", tag);
        return -1;
    }
    uint64_t ub = 0;
    uint64_t uspan = ReadVec(cfg + 0x00, &ub, 0x80 * 64);
    if (!uspan || uspan % 0x80 != 0) {
        Log("[slice] %s: parts span %llu not a multiple of 0x80 -- not shipped\n",
            tag, (unsigned long long)uspan);
        return -1;
    }
    *partsBase = ub;
    return (int)(uspan / 0x80);
}

// Everything after the leading entity field: the part count, one record per
// part, then the vehicle groups. Returns the group count (for the log line).
static int WriteVehicleConfig(FILE* f, uint64_t cfg, uint64_t ub, int units)
{
    fprintf(f, " %d", units);
    for (int k = 0; k < units; k++) {
        uint64_t u = ub + (uint64_t)k * 0x80;
        int32_t model = 0;
        memcpy(&model, (void*)(u + 0x00), 4);
        fprintf(f, " %d", model);
        uint64_t lb = 0;
        uint64_t lspan = ReadVec(u + 0x08, &lb, 0x400);
        int nl = (int)(lspan / 4);
        fprintf(f, " %d", nl);
        for (int j = 0; j < nl; j++) {
            int32_t v = 0; memcpy(&v, (void*)(lb + 4 * j), 4); fprintf(f, " %d", v);
        }
        float c[3] = { -1, -1, -1 };
        memcpy(c, (void*)(u + 0x20), 12);
        fprintf(f, " %.4f %.4f %.4f", c[0], c[1], c[2]);
        uint64_t ab = 0;
        uint64_t aspan = ReadVec(u + 0x60, &ab, 0x400);
        int na = (int)(aspan / 4);
        fprintf(f, " %d", na);
        for (int j = 0; j < na; j++) {
            int32_t v = 0; memcpy(&v, (void*)(ab + 4 * j), 4); fprintf(f, " %d", v);
        }
    }
    uint64_t gb = 0;
    uint64_t gspan = ReadVec(cfg + 0x18, &gb, 0x400);
    int ng = (int)(gspan / 4);
    fprintf(f, " %d", ng);
    for (int j = 0; j < ng; j++) {
        int32_t v = 0; memcpy(&v, (void*)(gb + 4 * j), 4); fprintf(f, " %d", v);
    }
    return ng;
}

static void WriteInjectVBuy(uint64_t depot, uint64_t cfg)
{
    uint64_t ub = 0;
    int units = VCfgParts(cfg, &ub, "VBUY");
    if (units < 0) return;
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return; }
    fprintf(f, "VBUY %d", (int)(int32_t)depot);
    int ng = WriteVehicleConfig(f, cfg, ub, units);
    fprintf(f, "\n");
    fclose(f);
    Log("[slice] VBUY shipped: depot=%d parts=%d groups=%d\n", (int)(int32_t)depot, units, ng);
}

// Vehicle commands that REFERENCE vehicles ship raw local entity ids; the Lua
// side turns them into cross-peer keys (a purchase's origin:seq, or s:<id> for
// a save vehicle) and the peer maps them back. Never cancelled.
//   VSELL  <n> <id..>            SellVehicle  (r8 = &vector<Entity>)
//   VDEPOT <vehicle> <sell01>    SendToDepot  (r8 = Entity, r9 = bool)
//   VLINE  <vehicle> <line> <stopIndex>   SetLine (r8, r9 = Entity, st[0] = int)
//   VREPL  <vehicle> <config..>  ReplaceVehicle (r8 = Entity, r9 = config*)
// VREPL's payload after the vehicle is byte-for-byte what VBUY writes after the
// depot -- the same TransportVehicleConfig, the same encoder -- so the Lua side
// builds the config for both lines with one function. Optimistic like the other
// vehicle commands: the UI waits for the replacement's result entity, so this is
// never cancelled and the originator skips its own replay.
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

static void WriteInjectVehicleCmd(int fid, uint64_t r8, uint64_t r9, uint64_t st0)
{
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return; }
    if (fid == 3) {
        uint64_t b = 0;
        uint64_t span = ReadVec(r8, &b, 0x400);
        int n = (int)(span / 4);
        fprintf(f, "VSELL %d", n);
        for (int i = 0; i < n; i++) { int32_t v = 0; memcpy(&v, (void*)(b + 4 * i), 4); fprintf(f, " %d", v); }
        fprintf(f, "\n");
        Log("[slice] VSELL shipped: %d vehicle(s)\n", n);
    } else if (fid == 4) {
        // ReplaceVehicle: r8 = the vehicle being replaced, r9 = the new
        // TransportVehicleConfig (a pointer, unlike BuyVehicle's by-value copy
        // on the caller's stack). A config that fails validation writes NOTHING
        // -- the line is only opened, never begun, so the file stays parseable.
        uint64_t ub = 0;
        int units = VCfgParts(r9, &ub, "VREPL");
        if (units >= 0) {
            fprintf(f, "VREPL %d", (int)(int32_t)r8);
            WriteVehicleConfig(f, r9, ub, units);
            fprintf(f, "\n");
            Log("[slice] VREPL shipped: vehicle=%d parts=%d\n", (int)(int32_t)r8, units);
        }
    } else if (fid == 5) {
        fprintf(f, "VDEPOT %d %d\n", (int)(int32_t)r8, (int)(r9 & 1));
        Log("[slice] VDEPOT shipped: vehicle=%d sell=%d\n", (int)(int32_t)r8, (int)(r9 & 1));
    } else if (fid == 6) {
        fprintf(f, "VLINE %d %d %d\n", (int)(int32_t)r8, (int)(int32_t)r9, (int)(int32_t)st0);
        Log("[slice] VLINE shipped: vehicle=%d line=%d stop=%d\n", (int)(int32_t)r8, (int)(int32_t)r9, (int)(int32_t)st0);
    } else if (fid == 7) {
        // CreateLine: the new line's content is read back from the entity by
        // the Lua side once it exists; only the EVENT ships from here.
        fprintf(f, "LCREATE\n");
        Log("[slice] LCREATE shipped\n");
    } else if (fid == 8) {
        fprintf(f, "LUPDATE %d\n", (int)(int32_t)r8);
        Log("[slice] LUPDATE shipped: line=%d\n", (int)(int32_t)r8);
    } else if (fid == 9) {
        fprintf(f, "LDELETE %d\n", (int)(int32_t)r8);
        Log("[slice] LDELETE shipped: line=%d\n", (int)(int32_t)r8);
    } else if (fid == 10) {
        fprintf(f, "VREV %d\n", (int)(int32_t)r8);
        Log("[slice] VREV shipped: vehicle=%d\n", (int)(int32_t)r8);
    } else if (fid == 13) {
        // SetColor(entity, Vec3f const&): r9 points at three floats, 0..1 each.
        float col[3] = { -1.0f, -1.0f, -1.0f };
        if (Readable((void*)r9, 12)) memcpy(col, (void*)r9, 12);
        if (col[0] >= 0.0f) {
            fprintf(f, "VCOLOR %d %.4f %.4f %.4f\n", (int)(int32_t)r8, col[0], col[1], col[2]);
            Log("[slice] VCOLOR shipped: entity=%d rgb=%.3f,%.3f,%.3f\n",
                (int)(int32_t)r8, col[0], col[1], col[2]);
        } else {
            Log("[slice] VCOLOR: colour at %llx unreadable -- not shipped\n", (unsigned long long)r9);
        }
    } else if (fid == 14) {
        // SetName(entity, std::string const&). MSVC layout: a 16-byte buffer,
        // size at +0x10, capacity at +0x18. The text sits inline while capacity
        // is 15 or less; past that, +0x00 is a pointer to it.
        char name[256]; name[0] = 0;
        if (Readable((void*)r9, 32)) {
            uint64_t len = 0, cap = 0;
            memcpy(&len, (void*)(r9 + 0x10), 8);
            memcpy(&cap, (void*)(r9 + 0x18), 8);
            const char* src = (const char*)r9;
            if (cap > 15) { uint64_t ptr = 0; memcpy(&ptr, (void*)r9, 8); src = (const char*)ptr; }
            if (len < sizeof(name) && src && Readable((void*)src, (size_t)len + 1)) {
                memcpy(name, src, (size_t)len); name[len] = 0;
            }
        }
        if (name[0]) {
            // Percent-encode: the wire is split on whitespace, and a player
            // names things "Coal Line 2".
            char enc[768]; size_t o = 0;
            for (size_t i = 0; name[i] && o + 4 < sizeof(enc); i++) {
                unsigned char ch = (unsigned char)name[i];
                if (ch > 32 && ch < 127 && ch != '%' && ch != '=') enc[o++] = (char)ch;
                else { sprintf(enc + o, "%%%02X", ch); o += 3; }
            }
            enc[o] = 0;
            fprintf(f, "VNAME %d %s\n", (int)(int32_t)r8, enc);
            Log("[slice] VNAME shipped: entity=%d name='%s'\n", (int)(int32_t)r8, name);
        } else {
            Log("[slice] VNAME: name at %llx unreadable or empty -- not shipped\n", (unsigned long long)r9);
        }
    }
    fclose(f);
}

static void CaptureFactory(const Factory& f, uint64_t rcx, uint64_t rdx, uint64_t r8,
                           uint64_t r9, uint64_t calleeRsp, uint64_t caller,
                           bool groundtruth, bool cancel)
{
    uint64_t st[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 6; i++)
        if (Readable((void*)(calleeRsp + 0x28 + 8 * i), 8))
            memcpy(&st[i], (void*)(calleeRsp + 0x28 + 8 * i), 8);

    Log("[cap] %s caller=%llx cmd=%llx rdx=%llx r8=%llx r9=%llx st=%llx %llx %llx %llx %llx %llx\n",
        f.name, (unsigned long long)caller, (unsigned long long)rcx,
        (unsigned long long)rdx, (unsigned long long)r8, (unsigned long long)r9,
        (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2],
        (unsigned long long)st[3], (unsigned long long)st[4], (unsigned long long)st[5]);

    // Real player buy (not a sweep): ship it. r9 = depot entity (value),
    // st[0] = pointer to the by-value config copy on the caller's stack.
    // Sell / Replace / SendToDepot / SetLine. The scripting layer's wrappers (our
    // own replays on the peer) live in one block, 0xcec000..0xcf2000 (ced378 =
    // buildProposal, cee710 = SetVehicleManualDeparture, ceefae = buyVehicle);
    // anything else is the UI. ReplaceVehicle (4) joins the list: it was hooked
    // for the ground-truth sweep only, so a player's "replace with this model"
    // reached the wire nowhere and the peer kept the old vehicle.
    // 13/14 (SetColor/SetName) ship through the same writer. Leaving them out
    // of this list meant the hook CAPTURED a rename -- '[cap] SetName' is in
    // the log -- and then wrote nothing, so renaming a line looked like a
    // replication failure when it never reached the wire at all.
    if (!groundtruth && ((f.id >= 3 && f.id <= 10) || f.id == 13 || f.id == 14)) {
        bool luaPath = (caller >= 0xcec000 && caller < 0xcf2000);
        if (luaPath) {
            Log("[slice] %s from the Lua path (caller=%llx) -- a replay, not shipped\n",
                f.name, (unsigned long long)caller);
        } else {
            WriteArmed(cancel && SessionLive());
            __try { WriteInjectVehicleCmd(f.id, r8, r9, st[0]); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[slice] %s decode fault -- not shipped\n", f.name); }
        }
    }

    if (f.id == 2 && !groundtruth && caller == CALLER_LUA_VEHICLE) {
        Log("[slice] VBUY from the Lua path (caller=%llx) -- a replay, not shipped\n",
            (unsigned long long)caller);
    } else if (f.id == 2 && !groundtruth) {
        __try {
            WriteInjectVBuy(r9, st[0]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[slice] VBUY decode fault -- buy proceeds locally, not shipped\n");
        }
    }

    if (groundtruth) {
        int testId = -1, sample = 0;
        uint64_t cand[9] = { rdx, r8, r9, st[0], st[1], st[2], st[3], st[4], st[5] };
        for (int i = 0; i < 9 && testId < 0; i++)
            if (cand[i] >= 900000 && cand[i] <= 999999) GtSentinel((double)cand[i], &testId, &sample);
        // SellVehicle (id 3): the sentinel is INSIDE the vector<Entity> -- the
        // first int behind [r8] (vector begin), never a register value (r6 E).
        if (testId < 0 && f.id == 3 && IsHeapPtr(r8)) {
            uint64_t vb = 0;
            if (ReadVec(r8, &vb, 0x400) >= 4) {
                int32_t first = 0;
                memcpy(&first, (void*)vb, 4);
                if (first >= 900000 && first <= 999999)
                    GtSentinel((double)first, &testId, &sample);
            }
        }
        if (testId >= 0) {
            Log("[gt] F%d test=%d sample=%d %s\n", f.id, testId, sample, f.name);
            GtDumpArg(f.id, "rdx", rdx, testId, sample);
            GtDumpArg(f.id, "r8",  r8,  testId, sample);
            GtDumpArg(f.id, "r9",  r9,  testId, sample);
            for (int i = 0; i < 6; i++) {
                char nm[8]; snprintf(nm, sizeof(nm), "s%d", i);
                GtDumpArg(f.id, nm, st[i], testId, sample);
            }
            GtFactoryDumps(f, rdx, r8, r9, st, testId, sample);
        }
    }

    if (cancel && SessionLive()) {
        InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
        InterlockedExchange(&g_pendingNoCb, 1);   // vehicle/line: no callback to honour
        Log("[slice] armed cancel: %s cmd=%llx (no-callback)\n", f.name, (unsigned long long)rcx);
    } else if (cancel) {
        Log("[slice] %s: no live session -- left alone, the game handles it\n", f.name);
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

static bool MergeTemplateStreet(uint64_t r8)
{
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
    int n = (int)((ne - nb) / 24), m = (int)((se - sb) / 120);
    if (n < 2 || n > 64 || m < 1 || m > 64) return false;
    if (!Readable((void*)nb, (size_t)(ne - nb)) || !Readable((void*)sb, (size_t)(se - sb))) return false;
    uint8_t* N = (uint8_t*)nb;
    uint8_t* S = (uint8_t*)sb;
    auto nodeId  = [&](int i) { int32_t v; memcpy(&v, N + i * 24 + 0x14, 4); return v; };
    auto nodeFl  = [&](int i) { uint32_t v; memcpy(&v, N + i * 24 + 0x0c, 4); return v; };

    // Template nodes = the placeholder endpoints of construction-OWNED segments
    // (+0x74 == 1). Node FLAGS are not a discriminator: for a TRACK template the
    // conversion stamps 0x7f00 on OUR node as well (rail depot dump 2026-08-30:
    // our -1 at index 0 already 0x7f00), so "first 0x7f00 node" saw no nodes of
    // ours and every rail depot replayed with the raw apron beside ours.
    bool isT[64] = {}; int nT = 0;
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
    (void)nodeFl;
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

// rax: 0 = let the original run, 1 = cancel it
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
    if (id == ID_BUILDPROPOSAL && CfgHas("dumpprop")) {
        Log("[slice] DUMPPROP(any) caller_rva=%llx\n", (unsigned long long)(retAddr - g_base));
        DumpProposal(3, r8, r9);
    }
    (void)rdx;
    uint64_t caller = retAddr - g_base;

    bool enabled = true, suppress = false, groundtruth = false;

    if (id == ID_CMDADD) {
        // Pointer match first: this runs ~100/sec and almost never matches.
        uint64_t want = (uint64_t)InterlockedCompareExchange64(&g_pendingCmd, 0, 0);
        if (!want || r8 != want) return 0;
        ReadCfg(&enabled, &suppress, nullptr);
        // A cancel must never outlive the switch that authorised it.
        if (!enabled) { InterlockedExchange64(&g_pendingCmd, 0); InterlockedExchange(&g_pendingNoCb, 0); return 0; }
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
                g_suppressed++;
                ZeroAddResult(rdx);
                Log("[slice] CANCEL fire-and-forget (caller_rva=%llx), callback "
                    "NOT fired -- avoids the false no-path toast\n", (unsigned long long)caller);
                return 1;
            }
            bool fired = false;
            if (Readable((void*)r9, 8) && Readable((void*)r8, 8)) {
                uint64_t vft = 0;
                memcpy(&vft, (void*)r9, 8);
                if (vft && Readable((void*)vft, 8 * 5)) {
                    uint64_t doCall = 0;
                    memcpy(&doCall, (void*)(vft + 0x10), 8);
                    if (doCall) {
                        __try {
                            ((void (*)(uint64_t, uint64_t))doCall)(r9, r8);
                            fired = true;
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            fired = false;
                        }
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
                    return 1;
                }
                Log("[slice] callback NOT fired -- letting the build run rather "
                    "than wedging the tool (caller_rva=%llx)\n",
                    (unsigned long long)caller);
                return 0;
            }
            InterlockedExchange(&g_pendingNoCb, 0);
            g_suppressed++;
            ZeroAddResult(rdx);
            Log("[slice] CANCEL local build (caller_rva=%llx), completion callback "
                "fired -- now owned by lockstep\n", (unsigned long long)caller);
            return 1;
        }
        Log("[slice] Add at %llx with no pending capture -- letting it run\n",
            (unsigned long long)caller);
        return 0;
    }

    if ((id >= 2 && id <= 10) || id == 13 || id == 14) {
        ReadCfg(&enabled, &suppress, &groundtruth);
        if (!enabled) return 0;
        const Factory* f = nullptr;
        for (int i = 0; i < NUM_FACTORIES; i++) if (FACTORIES[i].id == (int)id) f = &FACTORIES[i];
        if (!f) return 0;
        // cancel_line must NEVER arm for CreateLine (7) or UpdateLine (8),
        // regardless of cfg: cancelling CreateLine hands the UI
        // resultEntity=-1 (only the apply writes the real id) and its next
        // UpdateLine is a fatal assert -> game crash; cancelling UpdateLine
        // makes the next edit ship a stale snapshot (r8_analysis_lin.md 0,
        // DECOMPILED). Only SetLine (6) / DeleteLine (9) may consult it.
        // STRICT LOCKSTEP: cancel the UI-issued command so the originator
        // applies it at the SAME game-time stamp as the peer, not optimistically
        // at click time. NEVER cancel our own Lua-path replay (scripting block
        // 0xcec000..0xcf2000) -- that would cancel the replay we just issued.
        bool luaPath = (caller >= 0xcec000 && caller < 0xcf2000);
        bool cancel = false;
        // STRICT is safe ONLY for a command whose UI does not WAIT for a result.
        // Reverse (id 10) does not wait -- verified live, suppressing it cleanly
        // is fine. BuyVehicle (id 2) DOES wait: the depot window expects the new
        // vehicle entity back, and suppressing it without that result crashed
        // the client (assert, 2026-08-28, caller 74fda9). So only Reverse is
        // strict here; buy/sell/sendToDepot stay optimistic until each is shown
        // NOT to wait. CreateLine(7)/UpdateLine(8) can never be cancelled either.
        if (!luaPath && id == 10)
            cancel = CfgHas("cancel_vehicle");
        // SetLine (6) too. Measured 2026-08-31 with the APPLY log: every command
        // was issued on the identical sim step on both instances, yet the one
        // train's last departure differed by 0.8 s -- because the originator
        // assigned the line at CLICK time (optimistic) while the peer did it at
        // the stamp. A vehicle carries that offset for the rest of the game.
        // Strict here means both sides assign at the same step. The Lua only
        // replays on the originator when ARMED says the cancel happened, so a
        // cfg with this off stays consistent.
        else if (!luaPath && id == 6)
            cancel = CfgHas("cancel_line");
        __try {
            CaptureFactory(*f, rcx, rdx, r8, r9, calleeRsp, caller, groundtruth, cancel);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[slice] capture fault in %s -- proceeding\n", f->name);
        }
        return 0;
    }

    if (id != ID_BUILDPROPOSAL) return 0;

    ReadCfg(&enabled, &suppress, &groundtruth);
    if (!enabled) return 0;

    // Differential proposal dump (cfg 'dumpprop'): UI placement vs Lua replay.
    // dumpprop covers construction placements (0x419f62 UI, 0xced378 Lua) and,
    // as of 2026-08-29, the STREET/TRACK tool (0x459eb7) too: a native rail-over-
    // road crossing is only ever built by that tool, and its exact proposal
    // (which segments/nodes the UI submits at the crossing node) is the ground
    // truth the Lua replay has been unable to reproduce ("Collision").
    if ((caller == 0x419f62 || caller == 0xced378 || caller == 0x459eb7) && CfgHas("dumpprop"))
        DumpProposal(caller == 0x419f62 ? 1 : (caller == 0x459eb7 ? 3 : 2), r8, r9);

    // A Lua-issued construction proposal (our CONX replay): merge our shipped
    // apron INTO the template's connector so the engine sees the UI's shape.
    if (caller == 0xced378 && CfgHas("merge")) {
        __try {
            if (MergeTemplateStreet(r8) && CfgHas("dumpprop"))
                DumpProposal(3, r8, r9);            // post-merge, for the diff tool
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[merge] fault -- proposal left as built\n");
        }
    }

    // Bulldozer path (UI::Bulldozer::Apply): classify and LOG ONLY. Checked
    // before everything else so a bulldoze can never be mistaken for a road
    // capture or a ground-truth sample. Never cancelled in this commit.
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
            bool bEnabled = true, bSuppress = false;
            ReadCfg(&bEnabled, &bSuppress, nullptr);
            if (bEnabled && bSuppress && SessionLive()) {
                InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
                InterlockedExchange(&g_pendingNoCb, 0);
                Log("[slice] armed cancel: bulldoze cmd=%llx -- now owned by "
                    "lockstep, replays at the stamp\n", (unsigned long long)rcx);
            } else {
                Log("[slice] bulldoze shipped but not cancelled (enabled=%d "
                    "suppress=%d live=%d) -- it runs natively here and replays "
                    "on the peers\n", (int)bEnabled, (int)bSuppress, (int)SessionLive());
            }
        }
        return 0;
    }

    // Ground-truth samples come from Lua, so they arrive on a different caller
    // than the road path and must be checked BEFORE that filter rejects them.
    if (groundtruth && GroundTruthSample(r8, r9)) return 0;
    if (groundtruth && caller == 0x419f62) {
        // A real station placed by the player, chased the same way as a sweep
        // sample so the two can be compared path-for-path.
        Log("[gt] test=0 sample=0 PLAYER CONSTRUCTION caller=%llx\n", (unsigned long long)caller);
        ChaseStrings(0, 0, r9, 0x440);
        if (Readable((void*)(r9 + 0x1c0), 0x260))
            GtDumpRange("c", 0, 0, (const uint8_t*)(r9 + 0x1c0), 0x260, 0x1c0);
        return 0;
    }

    // Construction placement (caller 419f62): let it PROCEED untouched -- never
    // cancelled, never armed for cancel -- but ship its street vectors as a
    // ROADC companion so the peer can weld the replica into its road network.
    // The Lua side schedules the replay with skipOrigin: this instance's engine
    // has already integrated, so only the peer executes.
    if (caller == 0x419f62) {
        __try {
            Node cn[64];
            Edge ce[64];
            Edge crm[64];
            int n  = DecodeNodes(r8, cn, 64);
            int m  = DecodeEdges(r8, ce, 64);
            int re = DecodeEdges(r8 + 0x30, crm, 64);
            EdgeType cet = DecodeEdgeType(r8);
            if (m >= 1 && cet.ok) {
                g_conroad++;
                Log("[slice] #%ld construction placement: %d street node(s) %d "
                    "edge(s) %d removal(s), type=%s streetType=%d -- shipping ROADC\n",
                    g_conroad, n, m, re, cet.type == 1 ? "TRACK" : "street",
                    cet.streetType);
                WriteInjectConRoad(cn, n, ce, m, crm, re, cet);
            } else if (m >= 1) {
                Log("[slice] construction placement has %d street edge(s) but the "
                    "type decode failed -- NOT shipping ROADC (peer replica will "
                    "stay unconnected)\n", m);
            } else {
                Log("[slice] construction placement carries no street edges "
                    "(n=%d) -- free-standing, nothing to ship\n", n);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[slice] ROADC decode fault -- placement proceeds, nothing shipped\n");
        }
        return 0;
    }

    // The street/track UPGRADE tool takes the SAME path as the builder from here
    // on: same proposal struct, same decoders, same cancel-and-replay. Its shape
    // is the only difference (0 new nodes, N adds, N removals), and the branches
    // below say so where it matters.
    const bool isUpgrade = (caller == CALLER_UPGRADE);

    if (caller != CALLER_BUILDPROPOSAL && !isUpgrade) {
        // Log and move on. The previous version returned here in silence, so a
        // player reporting "I can't build anything" left NO trace at all -- there
        // was no way to tell a station attempt from a bulldoze from nothing
        // happening. A build is a rare event; logging every one costs nothing.
        Log("[slice] BuildProposal from caller_rva=%llx (not the road path) -- ignored\n",
            (unsigned long long)caller);
        return 0;
    }

    __try {
        Node nodes[256];
        Edge edges[512];
        int n = DecodeNodes(r8, nodes, 256);
        int m = DecodeEdges(r8, edges, 512);
        // Gate on EDGES, not new nodes. A road connecting two EXISTING junctions
        // adds ZERO new nodes (both endpoints already exist) and one edge whose
        // node0/node1 are positive existing ids. The old n<2 guard (correct for
        // the all-new-nodes ROADN format, wrong since ROADE) rejected exactly
        // that case: the build was not cancelled, so it happened LOCALLY and
        // never replicated -- observed as a one-edge desync in the two-way test.
        // The ROADE->ROADP converter already resolves positive endpoints to
        // positions via realPos(), so a 0-new-node road rebuilds on the peer.
        if (m < 1) {
            Log("[slice] %s capture: no edges (n=%d m=%d) -- not a build, "
                "letting it proceed\n", isUpgrade ? "upgrade" : "road", n, m);
            return 0;
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
        DumpFirstEdge(r8);
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
        if (isUpgrade && CfgHas("dumpprop")) {
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
        // rmEdges is 512 like the add vector: DecodeEdges silently CAPS at maxOut,
        // so a 64-slot buffer on an upgrade drag covering more than 64 segments
        // would ship every add against a truncated removal list -- the peer would
        // add edges on top of the ones it never removed.
        Node rmNodes[64];
        Edge rmEdges[512];
        int rn = DecodeNodes(r8 + 0x30, rmNodes, 64);
        int re = DecodeEdges(r8 + 0x30, rmEdges, 512);
        EdgeType et = DecodeEdgeType(r8);
        Log("[slice]   type=%s streetType=%d trackType=%d%s\n",
            et.type == 1 ? "TRACK" : "street", et.streetType, et.trackType,
            et.ok ? "" : "  <- DECODE FAILED, falling back to defaults");
        // Topology summary, then every int32 vector in the struct. The summary
        // is what makes the vectors interpretable: a junction that splits one
        // road must remove exactly one edge, so only a vector whose count
        // matches can be edgesToRemove.
        Log("[slice]   removed nodes=%d segs=%d (stride-correct)\n", rn, re);
        for (int i = 0; i < m && i < 12; i++)
            Log("[slice]     edge %d: %d -> %d  btype=%d bidx=%d%s\n", i,
                edges[i].node0, edges[i].node1, edges[i].btype, edges[i].bidx,
                edges[i].btype == 1 ? " (BRIDGE)" : edges[i].btype == 2 ? " (TUNNEL)" : "");
        DumpIdVectors(r8);

        // Inject ONLY when we are also cancelling. These were independent, and
        // that was a mistake: with suppression off the player's road was built
        // locally AND queued for replay, so it appeared twice on the originating
        // peer. Replication without cancellation is never what anyone wants, so
        // the two are now one decision.
        //
        //   enabled=0             -> inert
        //   enabled=1 suppress=0  -> observe and log only (safe to play with)
        //   enabled=1 suppress=1  -> capture, cancel, replicate (lockstep)
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
        if (suppress && !et.ok) {
            Log("[slice]   NOT cancelling: type decode failed, so this build "
                "cannot be replicated faithfully -- it stays local\n");
        } else if (suppress && isUpgrade && re < 1) {
            Log("[slice]   NOT cancelling: upgrade with %d added edge(s) decoded "
                "0 removals -- replaying the adds alone would duplicate every "
                "edge on the peer, so it stays local\n", m);
        } else if (suppress && isUpgrade && re < m) {
            // Fewer removals than adds means the peer would ADD edges over ones it
            // never removed (a decode cap, or a shape we have not seen). Never
            // cancel on data we cannot replay faithfully -- the same rule as a
            // failed type decode.
            Log("[slice]   NOT cancelling: upgrade has %d add(s) but only %d "
                "removal(s) -- would duplicate edges on the peer, stays local\n", m, re);
        } else if (suppress) {
            // Removed edges travel for the UPGRADE path only. The road tool's
            // splits are still shipped as re=0 and re-derived on each peer
            // (execPolyline splits its own copy); turning that on here would
            // change a working channel's behaviour in the same commit that adds
            // a new one, and a removal the peer cannot match now SKIPS the whole
            // command. Flip it once upgrades have proven the matcher.
            int shipRe = isUpgrade ? re : 0;
            const bool live = SessionLive();
            WriteArmed(live);
            WriteInject(nodes, n, edges, m, nullptr, 0, rmEdges, shipRe, et);
            if (isUpgrade && re > m)
                Log("[slice]   upgrade ships %d add(s) against %d removal(s) -- "
                    "more removals than adds, watch the peer\n", m, re);
            // Arm the cancel. The Add hook matches on the COMMAND POINTER, not
            // on a caller RVA, so the upgrade tool's own CommandList::Add call
            // site is recognised with no extra constant -- and its completion
            // callback is fired there like the build tool's (g_pendingNoCb stays
            // 0: this tool waits on the callback, so swallowing it would wedge
            // the upgrade cursor for the rest of the session).
            if (live) InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
            else Log("[slice] no live session (mod off, or nobody to replay it) -- the build runs natively\n");
        } else {
            Log("[slice]   suppress=0: observe-only, build proceeds normally "
                "and was NOT replicated\n");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[slice] capture faulted -- proceeding, never cancel on an error\n");
        InterlockedExchange64(&g_pendingCmd, 0);
        return 0;
    }
    return 0;
}

static void WriteBlob(uint8_t* p, int id, void* relay)
{
    int o = 0;
    p[o++] = 0x41; p[o++] = 0x52;                       // push r10
    p[o++] = 0x50;                                      // push rax
    p[o++] = 0xB8; memcpy(p + o, &id, 4); o += 4;       // mov eax, id
    p[o++] = 0x49; p[o++] = 0xBA; o += 8;               // mov r10, tramp (patched)
    p[o++] = 0x41; p[o++] = 0x52;                       // push r10
    p[o++] = 0x49; p[o++] = 0xBA;                       // mov r10, relay
    memcpy(p + o, &relay, 8); o += 8;
    p[o++] = 0x41; p[o++] = 0xFF; p[o++] = 0xE2;        // jmp r10
}

static bool Install(uint8_t* blob, uintptr_t rva, int steal, int id, const char* name)
{
    WriteBlob(blob, id, (void*)&DeferRelay);
    void* tramp = nullptr;
    if (!InstallHook(g_base + rva, blob, steal, &tramp)) {
        Log("[slice] HOOK FAILED %s rva=%llx\n", name, (unsigned long long)rva);
        return false;
    }
    memcpy(blob + 10, &tramp, 8);
    FlushInstructionCache(GetCurrentProcess(), blob, BLOB_SIZE);
    Log("[slice] hooked %s rva=%llx steal=%d id=%d\n",
        name, (unsigned long long)rva, steal, id);
    return true;
}

// The directory this DLL was loaded from, with a trailing backslash. Used
// only for the cfg lookup; everything written at run time goes to g_dataDir.
static void ResolveDllDir()
{
    HMODULE h = nullptr;
    wchar_t w[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&ResolveDllDir, &h) ||
        !GetModuleFileNameW(h, w, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(w, L'\\');
    if (!slash) return;
    slash[1] = 0;                                   // keep the backslash
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, g_dllDir, (int)sizeof(g_dllDir),
                            nullptr, nullptr) <= 0)
        g_dllDir[0] = 0;
}

// Compare the running exe's PE header against the build the RVAs were
// measured on. Reads IMAGE_DOS_HEADER -> e_lfanew -> IMAGE_NT_HEADERS64 in
// the mapped image, guarded by Readable() so a hostile or truncated header
// fails the check instead of faulting the attach thread. Reports what it
// found so a mismatch log names the actual build the player is running.
static bool GameBuildMatches(uintptr_t base, DWORD* stamp, DWORD* size)
{
    *stamp = 0; *size = 0;
    if (!base || !Readable((const void*)base, sizeof(IMAGE_DOS_HEADER))) return false;
    IMAGE_DOS_HEADER dos;
    memcpy(&dos, (const void*)base, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return false;
    uintptr_t nt = base + (uintptr_t)dos.e_lfanew;
    if (!Readable((const void*)nt, sizeof(IMAGE_NT_HEADERS64))) return false;
    IMAGE_NT_HEADERS64 hdr;
    memcpy(&hdr, (const void*)nt, sizeof(hdr));
    if (hdr.Signature != IMAGE_NT_SIGNATURE) return false;
    if (hdr.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    *stamp = hdr.FileHeader.TimeDateStamp;
    *size  = hdr.OptionalHeader.SizeOfImage;
    return *stamp == GAME_EXE_TIMEDATESTAMP && *size == GAME_EXE_SIZEOFIMAGE;
}

static DWORD WINAPI Init(LPVOID)
{
    HANDLE once = CreateMutexA(nullptr, TRUE, "tpf2_slice_hook_single_instance");
    if (once == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Directories first: nothing below can open a file until both are known.
    ResolveDllDir();
    if (!Tpf2mpDataDirA(g_dataDir, sizeof(g_dataDir), (const void*)&Init)) {
        g_dataDir[0] = 0;
        return 0;                                   // nowhere to log, nowhere to write
    }

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%stpf2_slice.log", g_dataDir);
    g_log = _fsopen(path, "w", _SH_DENYWR);
    if (!g_log) return 0;
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[slice] data dir=%s\n", g_dataDir);
    Log("[slice] dll dir=%s\n", g_dllDir[0] ? g_dllDir : "?");

    // BUILD GUARD -- before any RVA is patched.
    {
        DWORD stamp = 0, size = 0;
        if (!GameBuildMatches(g_base, &stamp, &size)) {
            Log("[slice] game build mismatch (stamp/size) -- hooks NOT installed: "
                "exe stamp=%08lx size=%08lx, RVAs measured on build %lu "
                "(stamp=%08lx size=%08lx)\n",
                (unsigned long)stamp, (unsigned long)size,
                (unsigned long)GAME_BUILD_NUMBER,
                (unsigned long)GAME_EXE_TIMEDATESTAMP,
                (unsigned long)GAME_EXE_SIZEOFIMAGE);
            return 0;
        }
        Log("[slice] game build ok: stamp=%08lx size=%08lx (build %lu)\n",
            (unsigned long)stamp, (unsigned long)size, (unsigned long)GAME_BUILD_NUMBER);
    }

    ReadInstance();
    bool en = true, sup = false, gt = false;
    ReadCfg(&en, &sup, &gt);
    Log("[slice] attached, base=%llx instance=%s suppress=%d\n",
        (unsigned long long)g_base, g_instance[0] ? g_instance : "?",
        sup ? 1 : 0);

    uint8_t* blobs = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blobs) { Log("[slice] blob alloc failed\n"); return 0; }

    // Order matters only for the log; the two targets do not overlap.
    Install(blobs, RVA_BUILDPROPOSAL, STEAL_BUILDPROPOSAL, ID_BUILDPROPOSAL, "BuildProposal");
    Install(blobs + BLOB_SIZE, RVA_CMDADD, STEAL_CMDADD, ID_CMDADD, "CommandList::Add");
    for (int i = 0; i < NUM_FACTORIES; i++)
        Install(blobs + BLOB_SIZE * (2 + i), FACTORIES[i].rva, FACTORIES[i].steal,
                FACTORIES[i].id, FACTORIES[i].name);

    for (;;) {
        Sleep(15000);
        Log("[slice] alive: captured=%ld cancelled=%ld addHits=%ld\n",
            g_captured, g_suppressed, g_addSeen);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
