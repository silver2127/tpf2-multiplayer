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
// ONE BEHAVIOUR, HARDCODED. While a session is live (SessionLive) a captured
// command is cancelled locally and replayed at the stamp on every instance;
// with no live session, or when a decode fails, it runs natively. A click on
// the clock's speed buttons is cancelled the same way and handed to the mod,
// whose leader makes it the session speed (CaptureSpeedButton). There is no
// observe mode and no per-channel switch: tpf2_slice.cfg carries only the
// dumpprop diagnostic, so a missing or garbled cfg cannot put this peer on a
// different protocol from the others.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <share.h>
#include <string>
#include <utility>
#include <vector>
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
// Clicking a bridge and confirming its replacement model uses a separate UI
// path. Live capture 2026-09-14: 0 new nodes, 10 added/10 removed bridge edges.
// Build 35924: 0x898680 calls BuildProposal, followed by CommandList::Add at
// 0x89869e. Use the same strict replacement path (including the callback) as
// the road/track upgrade brush; otherwise this applies only on the clicking peer.
static const uintptr_t CALLER_BRIDGE_UPGRADE = 0x898685;
// UI::StreetTerminalBuilder::commit -> make_cmd::BuildProposal return address.
// ONE tool covers roadside stops, rail signals and waypoints (measured
// 2026-09-08: all three placements arrived on this caller, shape addEdges=1
// rmEdges=1 -- the edge rebuilt with the object -- plus one edgeObjectsToAdd).
static const uintptr_t CALLER_STOPTOOL      = 0x460e0b;
// UI::Bulldozer::Apply's BuildProposal return address (r4_recon_dem.md A1:
// call at 0x3eb222, return addr 0x3eb227). LogBulldoze classifies it and ships
// what it can decode; the handler arms the cancel only when something shipped.
static const uintptr_t CALLER_BULLDOZE      = 0x3eb227;
// UI::ProposalAction::commit -> make_cmd::BuildProposal return address (call at
// 0x4311c1). Terraform, paint and the asset brush all commit through it, and
// their edit is the proposal TAIL, not its street half (docs/re/PROPOSALS.md
// "Terrain grids"). Observed only: logged, and saved to a file with dumpprop.
static const uintptr_t CALLER_PROPOSALACTION = 0x4311c6;
// The sol2 wrapper's factory call site (Lua path: api.cmd.make.buyVehicle).
// A BuyVehicle from HERE is our own replay on the peer: shipping it back
// would ping-pong purchases between the two instances forever. NOT 0x74fd88:
// that is the UI's buy (vehiclemanager.cpp, docs/re/COMMANDS.md) -- filtering it
// suppressed the player's real purchase (measured 2026-08-28). The cee***
// block is the scripting layer (cf. cee710 = SetVehicleManualDeparture's
// wrapper, ced378 = buildProposal's).
static const uintptr_t CALLER_LUA_VEHICLE   = 0xceefae;
// Lua replays that reach a factory from OUTSIDE the sol2 wrapper block: two
// makers are registration lambdas in gamescriptrep.cpp and still call their
// factory (measured live). api.cmd.make.setColor returns to 0xc3848e -- missed,
// every replayed VCOLOR was captured and shipped again until ~100,000 queued
// commands froze four games -- and api.cmd.make.setGameSpeed to 0xc17eff.
static bool IsScriptCaller(uint64_t caller)
{
    // 0xc17c79: api.cmd.make.createLine, another gamescriptrep.cpp lambda. Missing
    // here, every replayed LCREATE came back through the slice as a "UI" create --
    // an inert event until 2026-09-12, a cancelled replay once CreateLine is strict.
    return (caller >= 0xcec000 && caller < 0xcf2000) || caller == 0xc3848e || caller == 0xc17eff || caller == 0xc17c79;
}

static const int ID_BUILDPROPOSAL = 0;
static const int ID_CMDADD        = 1;
// SetGameSpeed (make_cmd 0x9de9e0, steal 21) is acted on for the clock
// widget's speed controls only, identified by the factory's return address
// (docs/re/COMMANDS.md): the speed buttons (0x4f0097, in 0x4eff50) and the pause
// toggle, whose two bodies are both UI::Clock::TogglePause (0x4efb8f in
// 0x4efab0, 0x4f26ef in 0x4f2640). The mod counts a speed button as the player's
// vote for the session speed and a toggle only as a pause or a resume -- a
// toggle's speed is just what the lever read before the pause -- so the inject
// line says which control it was.
// Every other caller (the menu switching to the game, CGameUI::GameStep, the
// camera-path tool, a debug view, and the Lua maker that pacing's own speed
// changes go through, which returns to 0xc17eff) is left alone.
static const int ID_SETGAMESPEED = 15;
static const uintptr_t CALLER_SPEED_BUTTONS[] = { 0x4efb8f, 0x4f0097, 0x4f26ef };
static const uintptr_t CALLER_PAUSE_TOGGLE[]  = { 0x4efb8f, 0x4f26ef };
// SetDate (make_cmd 0x9de9b0) and SetCalendarSpeed (0x9de870), steal 21 each:
// the same shape as SetGameSpeed, no Engine, the value in the low 32 bits of rdx.
// SetDate carries boost::gregorian's day number (the Julian Day Number: the
// editor builds it with date(y, m, d) at 0x2855e0 just before the call);
// SetCalendarSpeed carries milliseconds per day. Acted on for the editor's
// controls only, by return address: the date picker (0x4efe54) and the date
// speed slider (0x4f2af6). The Lua makers return to 0xcee8de and 0xc17e5e, and
// the mod replays through game.interface.setDate / setMillisPerDay, which call
// neither factory -- so a replay can never be captured again.
static const int ID_SETDATE          = 16;
static const int ID_SETCALENDARSPEED = 17;
static const uintptr_t CALLER_SET_DATE       = 0x4efe54;
static const uintptr_t CALLER_CALENDAR_SPEED = 0x4f2af6;
static const int BLOB_SIZE = 48;

// Every other command factory, same hook shape. Steal sizes are the ones
// args_probe ran against these functions live. ids 2..10, 13..17; 0 and 1 are above.
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
    { 0x9ddfe0, 20, 10, "Reverse",        "vehicle" },  // steal size: docs/re/COMMANDS.md
    { 0x9de8a0, 20, 13, "SetColor",       "sync"    },  // r9 -> CVec3f*, 3 floats
    { 0x9deb70, 15, 14, "SetName",        "sync"    },  // r9 -> std::string*, MSVC SSO
    { 0x9de9e0, 21, 15, "SetGameSpeed",   "speed"   },  // clock buttons only: CaptureSpeedButton
    { 0x9de9b0, 21, 16, "SetDate",          "calendar" },  // editor date picker only: CaptureCalendar
    { 0x9de870, 21, 17, "SetCalendarSpeed", "calendar" },  // editor date speed slider only: CaptureCalendar
    // The vehicle window's stop/go toggle. Its prologue is byte-for-byte
    // SendToDepot's (mov rax,rsp / push rdi / sub rsp,0xb70 / mov [rsp+40],-2 =
    // 20 bytes, checked in the 35924 exe, 2026-09-19): r8 = vehicle, r9 = bool.
    // A stopped train used to halt on the clicking game only and run on the
    // peers -- a position desync one stamp later.
    { 0x9df070, 20, 18, "SetUserStopped", "vehicle" },
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
// Set by CaptureFactory when it arms: ARMED 1 has ALREADY been written to the
// inject file, so the Lua will replay this command on the originator. If the
// completion callback then cannot be fired, letting the command run natively
// gives the player the action TWICE (7a29978: two vehicles for one click).
// With this set the Add hook honours the cancel anyway and says so; a window
// that needed the callback may need a refresh, which beats a double apply.
static volatile LONG g_pendingHonour = 0;
// Construction-placement cancel. The params walked off
// the PROPOSAL at the factory are stashed here and written as a CONXP record from
// the Add hook ONLY once the cancel actually landed -- if the completion callback
// cannot be fired and the build is let run, the stash is dropped and the entity
// poll captures the native build exactly as before. See StashConxpFromProposal.
static volatile LONG g_pendingIsConx = 0;
// Module edit / station upgrade: the same proposal shape
// carries the OLD construction in toRemove and the NEW ConstructionEntity in
// toAdd. Stashed at BuildProposal, shipped as CONUP from the Add hook only when
// the cancel lands -- otherwise the native upgrade runs and the entity poll
// ships it as a plain CONU exactly as before, so nothing can apply twice.
static volatile LONG g_pendingIsConu = 0;
static int32_t       g_conupOldId    = 0;
static bool StashConupFromProposal(uint64_t r8);   // defined with the CONUP writer below
static std::string g_conxpFile;
static float       g_conxpT[16];
// Growable, no cap (2026-09-16). A modular station with a dozen modules is
// ~9 KB of params; at the old 8 KB the walk truncated, the upgrade ran natively
// on the host only, and the peer rebuilt the station from a coalesced
// full-params edit -- 4 edges, the track heights and the price differed. 64 KB
// was the next cap. Now whatever the engine holds ships whole, or the walk
// refuses loudly and the placement runs natively with the notice.
static std::string g_conxpParams;
// Placement serial: one counter per process, stamped on a placement's ROADC
// (ps=) and on its CONXP (ps= rc=), so the Lua pairs the two by IDENTITY and
// not by which record happened to be read before which (cons.lua
// CM.flushConPairs). g_conxpSerial / g_conxpHadRoadc ride with the stash: the
// serial of the placement whose params are stashed, and whether a ROADC
// companion was written for it (rc=0: free-standing, no payload to wait for).
static long g_placeSerial   = 0;
static long g_conxpSerial   = 0;
static int  g_conxpHadRoadc = 0;
// Stop/signal/waypoint cancel. Decoded off the proposal's
// edgeObjectsToAdd record at the factory, written as STOPX from the Add hook
// only once the cancel landed (else dropped: the poll captures the native
// build, no double-capture). See StashStopFromProposal for the layout.
static volatile LONG g_pendingIsStop = 0;
static int32_t g_stopEid = 0, g_stopSide = 0, g_stopModel = 0, g_stopPlayer = 0;
static float   g_stopPos[3] = { 0, 0, 0 };
static uint8_t g_stopLeft = 0, g_stopOneWay = 0;
static std::string g_stopName;   // the stop's name at any length (the 255 this held cut a longer one)
// Stop/signal BULLDOZE cancel: the removed edge object,
// decoded off the bulldozer's edge-replace proposal (StashStopDelFromBulldoze),
// written as STOPXDEL from the Add hook once the cancel landed.
static volatile LONG g_pendingIsStopDel = 0;
// Terraform / paint cancel (STRICT, 2026-09-11): the blob is stashed at the
// factory and TERRAINCAP is written from the Add hook -- ARMED 1 when the
// cancel landed (everyone, the originator included, applies it at the stamp),
// ARMED 0 when the edit had to run natively here (the peers still get it).
static volatile LONG g_pendingIsTerrain = 0;
static char*    g_terrainB64 = nullptr;
static uint64_t g_terrainBlobLen = 0;
static long     g_terrainStashSeq = 0;
static bool     g_terrainIsPaint = false;   // the stashed edit paints only (no height grid)
// Asset brush cancel (STRICT, 2026-09-11): the same shape as the terrain one.
// The stroke is stashed at the factory as base64 plus the ids of the groups it
// removes; ASSETCAP is written from the Add hook behind ARMED 1 (cancelled, every
// instance applies it at the stamp) or ARMED 0 (it ran natively here).
static volatile LONG g_pendingIsAssets = 0;
static char*    g_assetB64 = nullptr;
static uint64_t g_assetBlobLen = 0;
static long     g_assetStashSeq = 0;
static std::string g_assetRemoveIds;          // "id,id,...": as long as the stroke needs
static int      g_assetRemoveCount = 0;
// THE STROKE WAITS FOR THE REPLAY (docs/re/PROPOSALS.md, Commit and apply).
// The terrain modifier commits mid-stroke (30 entries / 300k cells) and applies
// no brush while tool+0xf0 is set; its Add callback {vftable, tool, bool}
// clears +0xf0 in _Do_call. Firing that callback for a CANCELLED commit would
// release the stroke onto terrain that lacks the cancelled part, and the next
// part would be computed -- and shipped, absolute -- against the old heights.
// So after the fire the flag is set again and cleared only when this
// instance's own replay carrier (the Lua's empty buildProposal that
// InjectTerrainFromFile filled) has EXECUTED: Add only queues a command, it
// applies a sim step or more later and the tool's update runs in between, so
// the release rides a MARKER -- a second, empty Lua buildProposal that
// terrain.lua sends from the carrier's completion callback, which the factory
// sees only once the carrier has applied. A safety valve releases the tool
// after TERRAIN_HOLD_MAX_MS in case no marker ever comes.
static uint64_t g_terrainHeldTool = 0;
static ULONGLONG g_terrainHeldAt = 0;
static volatile LONG64 g_terrainCarrierCmd = 0;
static const ULONGLONG TERRAIN_HOLD_MAX_MS = 4000;

static int32_t g_stopDelEo = -1, g_stopDelEdge = -1;

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

// tpf2_slice.cfg. Every channel's behaviour is hardcoded (see the header); the
// one key this DLL still reads is the diagnostic `dumpprop`, default off.
//
// The file used to carry about twenty switches, matched by substring, with a
// separate built-in list for the no-file case -- and a lost or mangled cfg
// silently put a peer on a different protocol from the rest: a joiner with no
// cfg ran observe mode (2026-08-31), and a host whose MSI upgrade removed the
// file ran a whole session without the strict channels (2026-09-09). Nothing
// that changes what replicates may come from this file again.
//
// Lookup: the copy next to this DLL (where the installer puts it), then the
// data dir. First file found wins.
//
// Strict parse: a key counts only as an exact `key=0` or `key=1` at the very
// start of a line, optionally followed by blanks. Anything else -- a comment,
// leading blanks, spaces around '=', any other value, a line too long for the
// read buffer -- is ignored and the key keeps its default. The last valid line
// for a key wins.
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

static bool CfgFlag(const char* key, bool def)
{
    FILE* f = OpenCfg();
    if (!f) return def;
    const size_t klen = strlen(key);
    bool val = def;
    bool lineStart = true;            // does the next fgets chunk begin a line?
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const size_t len = strlen(line);
        const bool endsLine = len > 0 && line[len - 1] == '\n';
        const bool startsLine = lineStart;
        lineStart = endsLine;
        // A chunk that neither ends its line nor the file is a line too long
        // for the buffer: its tail was not seen, so it cannot be exact.
        if (!startsLine || !(endsLine || feof(f))) continue;
        if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
        const char v = line[klen + 1];
        if (v != '0' && v != '1') continue;
        const char* rest = line + klen + 2;
        while (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\n') rest++;
        if (*rest) continue;
        val = (v == '1');
    }
    fclose(f);
    return val;
}

// dumpprop, re-read at most every 2 s: it can still be flipped while the game
// runs, without opening the file on every proposal.
static bool DumpPropOn()
{
    static ULONGLONG lastRead = 0;
    static bool on = false;
    const ULONGLONG now = GetTickCount64();
    if (lastRead && now - lastRead < 2000) return on;
    lastRead = now;
    on = CfgFlag("dumpprop", false);
    return on;
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
        // The lobby's player count. Before the peer's first heartbeat "peer=?" says
        // nothing, yet the session is already multiplayer: two tracks laid 2 s after a
        // load ran natively on A only, and one replay then failed on B (2026-09-11).
        // A player count of 2 or more is live -- the replay half is running.
        const char* mk = strstr(line, "  mp=");
        if (!cached && mk) {
            int players = 0;
            if (sscanf(mk + 5, "%d", &players) == 1 && players >= 2) cached = true;
        }
    }
    fclose(f);
    return cached;
}

// Defined beside WriteArmed; the bulldozer's fallbacks below use it first.
static void WriteNativeNotice(const char* kind);

// ---------------------------------------------------------------------------
// VECTORS AT THE GAME'S OWN LENGTH.
//
// This DLL imposes no content limit of its own: a proposal, a vehicle config or
// an id list is read at whatever length the game holds. The fixed spans these
// readers used to carry (0x20000 B of road records, 64 vehicle parts, 256 sold
// vehicles, 16 demolished constructions, 16 edge objects) were not sanity checks
// but silent truncations: the command then ran natively on one instance and
// nowhere else, or shipped short and rebuilt short on every peer.
//
// The one bound left is a MISREAD-POINTER guard: a {begin,end} pair that spans
// more than VEC_SANE_SPAN is garbage read off the wrong offset, not a command
// (the largest real proposal, a whole-map terrain edit, is tens of MB). It logs
// the offending size and is treated as unreadable, which every caller handles
// as "cannot ship -- runs natively, NATIVE notice".
static const uint64_t VEC_SANE_SPAN = 512ull << 20;

enum VecRead { VEC_UNREADABLE, VEC_EMPTY, VEC_OK };

// The rest of this translation unit, in the order it always had, one region per file
// (native/src/slice/README.md). Each part is a plain continuation of this file: the same
// static symbols, the same forward declarations, the same relay contracts; the split is
// only so a region can be read on its own. Nothing here is a header: include order is
// the definition order, and build.bat still compiles slice_hook.cpp alone.
#include "slice/vectors_roads.inl"   // the game's vectors at their own length, road/rail node and edge decoding, the ROADE/EDEMO/CDEMO/ROADC records
#include "slice/strings_bulldoze.inl"   // heap pointer and std::string readers, percent encoding, the bulldozer's classification
#include "slice/vehicles.inl"   // vehicle configuration off a buy, VBUY, the ARMED and NATIVE notices
#include "slice/lines.inl"   // component::Line decoding, the platform assignment at replay, strict line creation and the spare line
#include "slice/capture.inl"   // the vehicle/line command writers, speed and calendar buttons, CaptureFactory (the factory hook's dispatcher)
#include "slice/constructions.inl"   // construction params off the proposal (CONXP), stops, signals, module edits, upgrade shape, proposal dumps
#include "slice/station_weld.inl"   // MergeTemplateStreet: a script-built station proposal shaped like the UI's (station_weld.h)
#include "slice/terrain_assets.inl"   // terrain tools and the asset brush: stash at the factory, inject from the Lua's file
#include "slice/add_hook.inl"   // the CommandList::Add hook: cancel, callbacks, stashes, DeferHandler
#include "slice/trainorder.inl"   // TRAIN RESERVATION ORDER (trainorder.h)
#include "slice/roadspace.inl"   // near-page detours, ROAD FREE SPACE and road entry order
#include "slice/sharedstations.inl"   // SHARED STATIONS: the line editor's owner gate in companies mode
#include "slice/ui_tints.inl"   // paused tick, icons for every player, company-colour tints on icons, labels and windows
#include "slice/sharedstations_install.inl"   // InstallSharedStations (the patch of the gate above)
#include "slice/moveorder.inl"   // SHIP AND AIRCRAFT CLAIM ORDER (moveorder.h)
#include "slice/init.inl"   // the relay blobs, hook installation, Init and DllMain
