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
struct Edge { int32_t node0, node1; float t0[3], t1[3]; int32_t btype, bidx; };

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

// VBUY: a player's BuyVehicle, shipped for replication. The config is decoded
// from the by-value TransportVehicleConfig on the caller's stack (st[0]):
// parts at +0x00 (0x80 stride: modelId +0x00, loadConfig +0x08, color +0x20,
// autoLoadConfig +0x60), vehicleGroups at +0x18 -- every offset a ground-truth
// EXACT match (docs/re/COMMANDS.md). The depot travels as its entity id; the Lua
// side on THIS instance turns it into a position and the model ids into file
// names before anything crosses to the peer. Cancelled and replayed at the
// stamp while a session is live (see the cancel decision in DeferHandler).
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
        fprintf(f, " %d", model);
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

// ---------------------------------------------------------------------------
// UpdateLine's component::Line, decoded at the factory.
//
// Until now LUPDATE shipped only the line id and every peer read the NEW stop
// list back from the entity after the command applied -- which is exactly why
// it could never be cancelled: cancel it and the entity still holds the OLD
// list. So the new list is read off the command's own Line, at entry, before
// the factory moves the stops vector out (r8 A NOTE).
//
// Layout. Ground-truth sweep (docs/re/COMMANDS.md, "Line::Stop"; the GT line
// sweep t10..t16, all EXACT unless noted; gt.lua is in git history at v0.4.11):
//   Line+0x00 vector<Stop> {begin,end,cap}   t10: span tracks 0xa8 per stop.
//              (An older note put the vector at +0x18; waitingTime is at
//              +0x18, and the dump that found the span read +0x00. +0x18 is
//              tried as a fallback only if +0x00 fails the shape check.)
//   Line+0x18 int waitingTime               t11 EXACT
//   Stop (0xa8): +0x04 int station (index in the group)  t13 EXACT
//                +0x08 int terminal                       t12 EXACT
//                +0x10 vector alternativeTerminals        t16 (span)
//                +0x28 int loadMode                       CONFIRMED: TransportVehicleSystem::Update2
//                      lambda (decomp tvs_update2_lambda.c) reads *(int*)(stop+0x28) and
//                      asserts "stop.loadMode == FULL_LOAD_ANY"; values 0..3 (0 and 3
//                      take no wait, 1 and 2 do).
//                +0x2c FLOAT a wait field                 t15 EXACT (min or max)
//                +0x30 FLOAT the other wait field         same lambda: *(float*)(stop+0x30)
//                      is compared against elapsed seconds, so both waits are floats
//                +0x38 vector waypoints                   (not shipped: lineSnapshot never did)
//                +0x00 Entity stationGroup                INFERRED (the only slot left
//                      before station); the Lua resolves it and refuses anything
//                      that is not a station group.
// Every unconfirmed field is range-checked; anything outside its range fails
// the decode, and a failed decode is NOT cancelled (the event-only line ships
// and the peers read back as before). The first stop's raw fields are logged
// on every decode so one real edit pins the predicted offsets.
struct LineAlt  { int32_t station, terminal; };                   // StationTerminal, 8 B (t16: span 8*i at stop+0x10)
// No limits of our own on a line: stops, a stop's platform choice (every
// platform of a modular station it may use) and its signal waypoints are
// whatever the game holds. The fixed arrays this had (8 platforms, a waypoint
// index under 64) failed the decode SILENTLY; the edit then ran natively on
// the host alone and the peers applied a read-back that carries neither, so
// the host's trains and the peers' took different routes (vehicle drift
// desync 2026-09-16). No bounds at all: a vector is read at the length the
// game holds (ReadVec still requires the memory to be readable), an index
// only has to be non-negative, and every refusal names its check in
// g_lineDecodeWhy.
struct LineWp   { int32_t entity, index; };                        // transport::SignalId
// waits are the engine's floats, any value it holds (the cargo-wait slider goes
// past the 36000 s this once refused, natively on the host only -- 2026-09-16)
struct LineStop { int32_t sg, station, terminal, loadMode; float minWait, maxWait; int nAlt; std::vector<LineAlt> alt; int nWp; std::vector<LineWp> wp; };
static const uint64_t LINE_ANY_SPAN = ~0ull;   // ReadVec's cap, not used as one
static char g_lineDecodeWhy[200] = "";
#define LINE_REFUSE(...) do { _snprintf_s(g_lineDecodeWhy, sizeof(g_lineDecodeWhy), _TRUNCATE, __VA_ARGS__); return false; } while (0)
struct LineDecode { float wait; int n; std::vector<LineStop> st; };
static LineDecode g_lineDecode;
static bool       g_lineDecodeOk = false;

static void WriteLineWaypoints(FILE* f, const LineDecode& d)
{
    bool first = true;
    for (int i = 0; i < d.n; i++) for (int w = 0; w < d.st[i].nWp; w++) {
        fprintf(f, "%s%d:%d:%d", first ? " wp=" : ",", i + 1,
                d.st[i].wp[w].entity, d.st[i].wp[w].index);
        first = false;
    }
}

static bool DecodeLineAt(uint64_t line, uint64_t vecOff, LineDecode* out)
{
    // An EMPTY stop list is a real edit: removing a line's last stop. ReadVec
    // rejects end == begin, so it used to fail the decode, stay uncancelled and
    // run natively on the clicker two steps before the peers' replay (a:7,
    // 2026-09-11 desync). Accepted only at +0x00 -- waitingTime at +0x18 has
    // already read sane by then -- and only as a well-formed empty vector.
    if (vecOff == 0x00 && Readable((void*)line, 0x18)) {
        uint64_t vb = 0, ve = 0, vc = 0;
        memcpy(&vb, (void*)line, 8);
        memcpy(&ve, (void*)(line + 0x08), 8);
        memcpy(&vc, (void*)(line + 0x10), 8);
        if (vb == ve && vc >= ve && (vb == 0 ? vc == 0 : IsHeapPtr(vb) && IsHeapPtr(vc))) {
            out->n = 0;
            out->st.clear();
            return true;
        }
    }
    uint64_t sb = 0;
    uint64_t span = ReadVec(line + vecOff, &sb, LINE_ANY_SPAN);
    if (span == 0 || (span % 0xa8) != 0) LINE_REFUSE("stops vector at +0x%llx: span %llu (0 or not a multiple of 0xa8)", (unsigned long long)vecOff, (unsigned long long)span);
    int n = (int)(span / 0xa8);
    if (n < 1) LINE_REFUSE("%d stops", n);
    out->n = n;
    out->st.clear();
    out->st.resize((size_t)n);
    for (int i = 0; i < n; i++) {
        const uint8_t* b = (const uint8_t*)sb + (size_t)i * 0xa8;
        LineStop& t = out->st[i];
        float f2c = 0.f, f30 = 0.f;
        memcpy(&t.sg,       b + 0x00, 4);
        memcpy(&t.station,  b + 0x04, 4);
        memcpy(&t.terminal, b + 0x08, 4);
        memcpy(&t.loadMode, b + 0x28, 4);
        memcpy(&f2c,        b + 0x2c, 4);
        memcpy(&f30,        b + 0x30, 4);
        // floats (see the layout note); only NaN is refused -- a huge, infinite
        // or negative wait is the slider's own encoding, shipped as %.9g
        if (f2c != f2c || f30 != f30) LINE_REFUSE("stop %d waits %g/%g", i + 1, f2c, f30);
        // +0x2c is minWaitingTime and +0x30 maxWaitingTime: the API's field
        // order (stationGroup, station, terminal, alternativeTerminals,
        // loadMode, minWaitingTime, maxWaitingTime, waypoints) laid out in
        // memory. This used to SORT the two ("min <= max always holds"), and
        // a max below the min -- the cargo slider's unlimited wait -- came out
        // swapped on every peer (2026-09-16: "min and max confused").
        t.minWait = f2c; t.maxWait = f30;
        if (t.sg <= 0 || t.station < 0 || t.terminal < 0
            || t.loadMode < 0 || t.loadMode > 3)
            LINE_REFUSE("stop %d sg=%d station=%d terminal=%d loadMode=%d", i + 1, t.sg, t.station, t.terminal, t.loadMode);
        // alternativeTerminals: vector<StationTerminal> at stop+0x10, 8 B each
        // (t16). Platform choice in the line editor lives here; a stop may
        // list several. Capped at 8; more than that fails the decode.
        t.nAlt = 0;
        uint64_t ab = 0;
        uint64_t aspan = ReadVec((uint64_t)b + 0x10, &ab, LINE_ANY_SPAN);
        if (aspan % 8) LINE_REFUSE("stop %d alternative terminals span %llu", i + 1, (unsigned long long)aspan);
        int na = (int)(aspan / 8);
        t.alt.assign((size_t)na, LineAlt{});
        for (int a = 0; a < na; a++) {
            memcpy(&t.alt[a].station,  (const uint8_t*)ab + a * 8 + 0, 4);
            memcpy(&t.alt[a].terminal, (const uint8_t*)ab + a * 8 + 4, 4);
            if (t.alt[a].station < 0 || t.alt[a].terminal < 0)
                LINE_REFUSE("stop %d alternative %d: station=%d terminal=%d", i + 1, a + 1, t.alt[a].station, t.alt[a].terminal);
        }
        t.nAlt = na;
        // vector<transport::SignalId> {entity,index}, after this station stop.
        uint64_t wb = 0, we = 0;
        memcpy(&wb, b + 0x38, 8); memcpy(&we, b + 0x40, 8);
        if (we < wb || (we - wb) % 8) LINE_REFUSE("stop %d waypoint vector %llx..%llx", i + 1, (unsigned long long)wb, (unsigned long long)we);
        t.nWp = (int)((we - wb) / 8);
        if (t.nWp && !Readable((void*)wb, (size_t)(we - wb))) LINE_REFUSE("stop %d waypoints unreadable", i + 1);
        t.wp.assign((size_t)t.nWp, LineWp{});
        for (int w = 0; w < t.nWp; w++) {
            memcpy(&t.wp[w], (void*)(wb + w * 8), 8);
            if (t.wp[w].entity <= 0 || t.wp[w].index < 0) LINE_REFUSE("stop %d waypoint %d: entity=%d index=%d", i + 1, w + 1, t.wp[w].entity, t.wp[w].index);
        }
    }
    return true;
}

static bool DecodeLine(uint64_t line, LineDecode* out)
{
    g_lineDecodeWhy[0] = 0;
    if (!IsHeapPtr(line) || !Readable((void*)line, 0x24)) LINE_REFUSE("line struct unreadable");
    // waitingTime is a FLOAT at +0x18 (every live capture said so); any value
    // but NaN is the game's own
    {
        float wf = 0.f;
        memcpy(&wf, (void*)(line + 0x18), 4);
        if (wf != wf) LINE_REFUSE("waitingTime NaN");
        out->wait = wf;
    }
    if (DecodeLineAt(line, 0x00, out)) return true;
    if (DecodeLineAt(line, 0x18, out)) { Log("[slice] LUPDATE: stops vector found at +0x18, not +0x00 -- update the layout note\n"); return true; }
    return false;
}

// ---------------------------------------------------------------------------
// STRICT LINE CREATION (2026-09-12).
//
// A line the player creates used to exist on their own game one command delay
// before anyone else's: CreateLine was never cancelled, because both UI callers'
// completion callbacks (UI::LineList 0x610490, UI::LineManager 0x6154a0) read the
// new line off the command and assert "resultEntity != ecs::Entity()" when it is
// empty -- a cancel with the callback fired is a fatal assert. Created early and
// natively, the line took a different entity id on the originator, entity ids
// diverged from there, and the worlds split (3-game rig, 2026-09-12: people at the
// very next sample, then town buildings).
//
// Now the UI's CreateLine is decoded (name, colour, component::Line), shipped as
// LCREATEX behind ARMED 1 and cancelled WITHOUT firing its callback: the callback
// object is MOVED into a stash instead. At the stamp the originator's Lua replays
// the create like every peer, after writing lockstep_lclaim_<x>.txt; the factory
// hook sees that claimed createLine, and the CommandList::Add hook puts the
// stashed callback in place of the Lua's (the relay's pushed r9 at calleeRsp-0x38,
// deferrelay_slice.asm). The line editor then gets its real result -- the line
// created on the same step as everywhere else -- a fraction of a second later.
static const uintptr_t CALLER_UI_CREATELINE = 0x215c26b;   // line_util, used by both the line list and the line manager
struct LineCreateDecode { std::string nameEnc; float rgb[3]; LineDecode line; };   // the name at any length, percent-encoded
static LineCreateDecode g_lcDecode;
static bool g_lcDecodeOk = false;
struct LcStash { uint8_t* fn; ULONGLONG at; };
static SRWLOCK g_lcLock = SRWLOCK_INIT;
// Held UI callbacks, oldest first, as many as the player creates lines: the
// eight this once held dropped the ninth callback of a burst of creates ("stash
// full"), and that line's editor never got its result.
static std::vector<LcStash> g_lcStash;
static volatile LONG g_pendingStashCb = 0;        // the pending cancel is a CreateLine: stash its callback at Add
static volatile LONG64 g_lcCarrierCmd = 0;        // our claimed Lua createLine, whose Add takes the stashed callback
// The factory's `this` is NOT what reaches Add for a Lua create: api.cmd.sendCommand
// copies the command into its own object first (2026-09-16: every claim logged
// "carries the line editor's callback", none ever "rides on our replay", and the
// line editor never got its new line). The replay's Add is recognised instead as
// the first Add from sendCommand's own call site (docs/re/COMMANDS.md: returns to
// 0x1126f1a) on the thread that made the claim -- one Lua statement, no other Add
// between the factory and it.
static const uintptr_t CALLER_SCRIPT_SENDCOMMAND = 0x1126f1a;
static volatile LONG g_lcCarrierTid = 0;
static uint8_t* g_lcCarrierFn = nullptr;          // the std::function object handed to that Add
static uint8_t* g_lcSpentFn = nullptr;            // the previous carrier's object, emptied by Add; freed on the next swap
static long g_lcClaimSeen = 0;

static bool DecodeLineCreate(uint64_t rdx, uint64_t r8, uint64_t st0)
{
    g_lcDecodeOk = false;
    // name: the UI's std::string at rdx, any length (ReadStdString). Empty is
    // refused: the Lua's LCREATEX parser needs a name token (name=%S+).
    std::string name;
    if (!ReadStdString(rdx, &name, "CreateLine name")) { Log("[slice] CreateLine: name at %llx unreadable\n", (unsigned long long)rdx); return false; }
    if (name.empty()) { Log("[slice] CreateLine: empty name -- not decodable for the wire\n"); return false; }
    g_lcDecode.nameEnc = PercentEncode(name);   // like VNAME: the wire splits on whitespace
    // colour: r8 -> three floats
    if (!Readable((void*)r8, 12)) return false;
    memcpy(g_lcDecode.rgb, (void*)r8, 12);
    for (int i = 0; i < 3; i++) if (!(g_lcDecode.rgb[i] >= 0.0f && g_lcDecode.rgb[i] <= 1.0f)) return false;
    // the line: st[0] -> component::Line, the same layout UpdateLine carries
    if (!DecodeLine(st0, &g_lcDecode.line)) return false;
    g_lcDecodeOk = true;
    return true;
}

// Move the UI's std::function (0x40 bytes, impl pointer at +0x38) into a heap object
// of our own, the way MSVC's own move does: a functor stored inside the object is
// moved with its _Move (vftable +0x08) into ours; a heap impl is taken by pointer.
static bool StashLineCreateCallback(uint64_t r9)
{
    if (!Readable((void*)r9, 0x40)) return false;
    uint64_t impl = 0;
    memcpy(&impl, (void*)(r9 + 0x38), 8);
    if (!impl || !Readable((void*)impl, 8)) return false;
    uint8_t* buf = (uint8_t*)calloc(1, 0x40);
    if (!buf) return false;
    bool ok = false;
    __try {
        if (impl == r9) {
            uint64_t vft = 0, moveFn = 0;
            memcpy(&vft, (void*)impl, 8);
            if (vft && Readable((void*)vft, 0x28)) memcpy(&moveFn, (void*)(vft + 0x08), 8);
            if (moveFn >= g_base && moveFn < g_base + GAME_EXE_SIZEOFIMAGE) {
                const uint64_t moved = ((uint64_t (*)(uint64_t, uint64_t))moveFn)(impl, (uint64_t)buf);
                memcpy(buf + 0x38, &moved, 8);
                ok = moved != 0;
            }
        } else {
            memcpy(buf + 0x38, &impl, 8);
            const uint64_t zero = 0;
            memcpy((void*)(r9 + 0x38), &zero, 8);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) { free(buf); return false; }
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lcLock);
    // a stash no replay claimed within a minute is dropped (leaked, never called)
    size_t k = 0;
    for (size_t i = 0; i < g_lcStash.size(); i++) if (now - g_lcStash[i].at < 60000) g_lcStash[k++] = g_lcStash[i];
    g_lcStash.resize(k);
    g_lcStash.push_back(LcStash{ buf, now });
    ReleaseSRWLockExclusive(&g_lcLock);
    return true;
}

// Our Lua is about to create a line: if it wrote a fresh claim, this createLine is
// the originator's own replay, and the oldest stashed UI callback rides on its Add.
static void ClaimLineCreateCarrier(uint64_t rcx)
{
    ReadInstance();
    if (!g_instance[0]) return;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_lclaim_%s.txt", g_dataDir, g_instance);
    // Fresh claims only: the Lua writes it in the same call that makes this
    // command, so a claim older than a few seconds is left over (a peer's line
    // replayed later must not carry our editor's callback).
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) return;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const uint64_t wrote = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    const uint64_t now = ((uint64_t)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    if (now > wrote && now - wrote > 5ULL * 10000000ULL) return;
    FILE* f = _fsopen(p, "r", _SH_DENYNO);
    if (!f) return;
    long claim = 0;
    if (fscanf(f, "%ld", &claim) != 1) claim = 0;
    fclose(f);
    if (claim <= 0 || claim == g_lcClaimSeen) return;
    g_lcClaimSeen = claim;
    uint8_t* fn = nullptr;
    AcquireSRWLockExclusive(&g_lcLock);
    if (!g_lcStash.empty()) {
        fn = g_lcStash.front().fn;
        g_lcStash.erase(g_lcStash.begin());
    }
    ReleaseSRWLockExclusive(&g_lcLock);
    if (!fn) { Log("[slice] CreateLine: claim %ld but no held callback -- the replay runs with the Lua's own\n", claim); return; }
    if (g_lcCarrierFn) Log("[slice] CreateLine: a previous carrier never reached Add -- its callback is dropped\n");
    g_lcCarrierFn = fn;
    InterlockedExchange(&g_lcCarrierTid, (LONG)GetCurrentThreadId());
    InterlockedExchange64(&g_lcCarrierCmd, (LONG64)rcx);
    Log("[slice] CreateLine: claim %ld -- our replay cmd=%llx carries the line editor's callback\n", claim, (unsigned long long)rcx);
}

// At that Add: hand the engine our std::function instead of the Lua's. Checked against
// the relay frame first: the pushed r9 must be the r9 we were called with.
static void SwapInLineCreateCallback(uint64_t r9, uint64_t calleeRsp)
{
    uint8_t* fn = g_lcCarrierFn;
    g_lcCarrierFn = nullptr;
    if (!fn) return;
    const uint64_t slot = calleeRsp - 0x38;   // deferrelay_slice.asm: push rcx, rdx, r8, r9 from entry rsp = calleeRsp - 0x18
    uint64_t saved = 0;
    if (!Readable((void*)slot, 8)) { Log("[slice] CreateLine: relay frame unreadable -- callback not swapped\n"); return; }
    memcpy(&saved, (void*)slot, 8);
    if (saved != r9) { Log("[slice] CreateLine: relay frame holds %llx, not r9 %llx -- callback not swapped\n", (unsigned long long)saved, (unsigned long long)r9); return; }
    if (g_lcSpentFn) free(g_lcSpentFn);
    const uint64_t v = (uint64_t)fn;
    memcpy((void*)slot, &v, 8);
    g_lcSpentFn = fn;
    Log("[slice] CreateLine: the line editor's callback now rides on our replay\n");
}

// The buy's completion callback. The depot window's buy and the vehicle manager's
// CLONE share it: _Do_call 0x753820 runs 0x748250 on the lambda at impl+8, and that
// lambda's int at +0x30 is the line a clone puts the new vehicle on (SetLine via
// 0x88b840); below 0 it is a plain depot buy and opens the vehicle window instead.
// Cancelled, the command has no result vehicle, so the clone's SetLine never came:
// "the clone button does nothing" (2026-09-11). The line is shipped as VBUYLINE right
// behind the VBUY, and the replay assigns the vehicle on every instance.
static const uintptr_t RVA_BUY_CALLBACK_THUNK = 0x753820;

static void WriteInjectBuyLine(int32_t line)
{
    ReadInstance();
    if (!g_instance[0]) return;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s -- clone line %d not shipped\n", p, line); return; }
    fprintf(f, "VBUYLINE %d\n", line);
    fclose(f);
    Log("[slice] VBUYLINE shipped: the cancelled buy was a clone onto line %d\n", line);
}

// Returns true when a line was written.
static bool WriteInjectVehicleCmd(int fid, uint64_t r8, uint64_t r9, uint64_t st0)
{
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return false; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return false; }
    bool shipped = true;
    if (fid == 3) {
        // the whole list: a bulk sell of a big fleet is ONE command with every
        // vehicle in it (256 was the old cut, and the rest stayed unsold on the peers)
        std::vector<int32_t> ids;
        if (ReadIntVec(r8, &ids, "VSELL") && !ids.empty()) {
            fprintf(f, "VSELL %d", (int)ids.size());
            for (size_t i = 0; i < ids.size(); i++) fprintf(f, " %d", ids[i]);
            fprintf(f, "\n");
            Log("[slice] VSELL shipped: %d vehicle(s)\n", (int)ids.size());
        } else {
            Log("[slice] VSELL: vehicle list unreadable or empty -- not shipped\n");
            shipped = false;
        }
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
        } else {
            shipped = false;
        }
    } else if (fid == 5) {
        fprintf(f, "VDEPOT %d %d\n", (int)(int32_t)r8, (int)(r9 & 1));
        Log("[slice] VDEPOT shipped: vehicle=%d sell=%d\n", (int)(int32_t)r8, (int)(r9 & 1));
    } else if (fid == 6) {
        fprintf(f, "VLINE %d %d %d\n", (int)(int32_t)r8, (int)(int32_t)r9, (int)(int32_t)st0);
        Log("[slice] VLINE shipped: vehicle=%d line=%d stop=%d\n", (int)(int32_t)r8, (int)(int32_t)r9, (int)(int32_t)st0);
    } else if (fid == 7) {
        if (g_lcDecodeOk) {
            // LCREATEX <r> <g> <b> <wait> <n> {<sg> <station> <terminal> <loadMode> <min> <max> <nAlt> {<st> <term>}*nAlt}*n name=<enc>
            // The colour goes out EXACT: %.9g round-trips a float. The line editor gives a
            // new line the least-used lineColors entry (bright ones first), counting the
            // existing lines' colours by exact float equality (FUN_14215da30). Rounded to
            // %.4f, 127/255 came back as 0.4980, matched no palette entry, and every new
            // line was the same orange (2026-09-12).
            const LineDecode& d = g_lcDecode.line;
            fprintf(f, "LCREATEX %.9g %.9g %.9g %.9g %d", g_lcDecode.rgb[0], g_lcDecode.rgb[1], g_lcDecode.rgb[2], d.wait, d.n);
            for (int i = 0; i < d.n; i++) {
                fprintf(f, " %d %d %d %d %.9g %.9g %d", d.st[i].sg, d.st[i].station, d.st[i].terminal,
                        d.st[i].loadMode, d.st[i].minWait, d.st[i].maxWait, d.st[i].nAlt);
                for (int a = 0; a < d.st[i].nAlt; a++)
                    fprintf(f, " %d %d", d.st[i].alt[a].station, d.st[i].alt[a].terminal);
            }
            WriteLineWaypoints(f, d);
            fprintf(f, " name=%s\n", g_lcDecode.nameEnc.c_str());
            Log("[slice] LCREATEX shipped: name=%.200s stops=%d\n", g_lcDecode.nameEnc.c_str(), d.n);
        } else {
            // not decoded: the new line's content is read back from the entity by
            // the Lua side once it exists; only the EVENT ships from here.
            fprintf(f, "LCREATE\n");
            Log("[slice] LCREATE shipped (event only)\n");
        }
    } else if (fid == 8) {
        if (g_lineDecodeOk) {
            // LUPDATE <line> <wait> <n> {<sg> <station> <terminal> <loadMode> <min> <max> <nAlt> {<st> <term>}*nAlt}*n
            const LineDecode& d = g_lineDecode;
            fprintf(f, "LUPDATE %d %.9g %d", (int)(int32_t)r8, d.wait, d.n);
            for (int i = 0; i < d.n; i++) {
                fprintf(f, " %d %d %d %d %.9g %.9g %d", d.st[i].sg, d.st[i].station, d.st[i].terminal,
                        d.st[i].loadMode, d.st[i].minWait, d.st[i].maxWait, d.st[i].nAlt);
                for (int a = 0; a < d.st[i].nAlt; a++)
                    fprintf(f, " %d %d", d.st[i].alt[a].station, d.st[i].alt[a].terminal);
            }
            WriteLineWaypoints(f, d);
            fprintf(f, "\n");
            if (d.n > 0)
                Log("[slice] LUPDATE shipped DECODED: line=%d wait=%g stops=%d (first: sg=%d st=%d term=%d lm=%d wait=%g..%g)\n",
                    (int)(int32_t)r8, d.wait, d.n, d.st[0].sg, d.st[0].station, d.st[0].terminal,
                    d.st[0].loadMode, d.st[0].minWait, d.st[0].maxWait);
            else
                Log("[slice] LUPDATE shipped DECODED: line=%d wait=%g stops=0 (last stop removed)\n",
                    (int)(int32_t)r8, d.wait);
        } else {
            fprintf(f, "LUPDATE %d\n", (int)(int32_t)r8);
            Log("[slice] LUPDATE shipped (event only): line=%d\n", (int)(int32_t)r8);
        }
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
            fprintf(f, "VCOLOR %d %.9g %.9g %.9g\n", (int)(int32_t)r8, col[0], col[1], col[2]);   // exact, like LCREATEX's colour
            Log("[slice] VCOLOR shipped: entity=%d rgb=%.3f,%.3f,%.3f\n",
                (int)(int32_t)r8, col[0], col[1], col[2]);
        } else {
            Log("[slice] VCOLOR: colour at %llx unreadable -- not shipped\n", (unsigned long long)r9);
            shipped = false;
        }
    } else if (fid == 14) {
        // SetName(entity, std::string const&): the name at any length
        // (ReadStdString), percent-encoded because the wire is split on
        // whitespace and a player names things "Coal Line 2". An EMPTY name
        // cannot travel: the Lua's VNAME parser needs a third token.
        std::string name;
        if (!ReadStdString(r9, &name, "VNAME")) name.clear();
        if (!name.empty()) {
            const std::string enc = PercentEncode(name);
            fprintf(f, "VNAME %d %s\n", (int)(int32_t)r8, enc.c_str());
            Log("[slice] VNAME shipped: entity=%d name='%.200s'%s\n", (int)(int32_t)r8, name.c_str(),
                name.size() > 200 ? "..." : "");
        } else {
            Log("[slice] VNAME: name at %llx unreadable or empty -- not shipped\n", (unsigned long long)r9);
            shipped = false;
        }
    }
    fclose(f);
    return shipped;
}

// A cancel is only honest when the command's whole payload reaches the wire:
// BuyVehicle's and ReplaceVehicle's new config, SellVehicle's vehicle list.
// When that does not read, nothing can ship, so the command must run natively
// rather than be cancelled into nothing (never cancel on a failed decode). The
// other vehicle and line commands carry plain values.
static bool VehiclePayloadReadable(int fid, uint64_t r8, uint64_t r9, uint64_t st0)
{
    uint64_t b = 0;
    if (fid == 2) return VCfgParts(st0, &b, "VBUY") >= 0;
    if (fid == 3) { std::vector<int32_t> ids; return ReadIntVec(r8, &ids, "VSELL") && !ids.empty(); }
    if (fid == 4) return VCfgParts(r9, &b, "VREPL") >= 0;
    return true;
}

// A click on the clock's speed controls while a session is live: cancelled
// fire-and-forget (the clock reads the speed back every frame; nothing waits on
// the command) and written as SPEEDBTN <speed> <toggle|button>. The mod makes a
// speed button this player's vote for the session speed, which every instance
// counts at its stamp, and the host's pause toggle a pause or a resume, so a
// lever only moves through pacing. Not live, not a clock caller, or a value out
// of range: the click runs natively, as in a stock game.
static bool WriteInjectSpeedButton(int speed, const char* kind)
{
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return false; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return false; }
    fprintf(f, "SPEEDBTN %d %s\n", speed, kind);
    fclose(f);
    return true;
}

static void CaptureSpeedButton(uint64_t rcx, uint64_t rdx, uint64_t caller)
{
    const int speed = (int)(int32_t)(uint32_t)rdx;   // no Engine argument: the speed is the low 32 bits of rdx
    bool button = false, toggle = false;
    for (uintptr_t c : CALLER_SPEED_BUTTONS) if (caller == c) button = true;
    for (uintptr_t c : CALLER_PAUSE_TOGGLE) if (caller == c) toggle = true;
    if (!button) {
        static uint64_t seen[8] = {};
        for (int i = 0; i < 8; i++) {
            if (seen[i] == caller) break;
            if (!seen[i]) {
                seen[i] = caller;
                Log("[slice] SetGameSpeed(%d) from caller_rva=%llx -- not a speed button, left alone (logged once per caller)\n",
                    speed, (unsigned long long)caller);
                break;
            }
        }
        return;
    }
    if (speed < 0 || speed > 64) {
        Log("[slice] speed button value %d out of range -- left alone\n", speed);
        return;
    }
    if (!SessionLive()) {
        Log("[slice] speed button %d: no live session -- left alone\n", speed);
        return;
    }
    const char* kind = toggle ? "toggle" : "button";
    if (!WriteInjectSpeedButton(speed, kind)) {
        Log("[slice] speed button %d: not shipped -- left alone\n", speed);
        return;
    }
    InterlockedExchange(&g_pendingNoCb, 1);
    InterlockedExchange(&g_pendingHonour, 0);
    InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
    Log("[slice] armed cancel: speed %s %d (caller_rva=%llx) -- the mod applies it\n",
        kind, speed, (unsigned long long)caller);
}

// The editor's date picker and date speed slider while a session is live:
// cancelled fire-and-forget (the clock reads the date and calendar speed back
// every frame; nothing waits on the command) and written as SETDATE <julian day>
// or CALSPEED <ms per day>. Every instance, the originator included, applies it
// at the stamp (CM.execCalendar), so the calendar moves on the same sim step
// everywhere. Not live, not the editor, or a value out of range: it runs
// natively, as in a stock game.
static void CaptureCalendar(uint64_t id, uint64_t rcx, uint64_t rdx, uint64_t caller)
{
    const bool isDate = (id == (uint64_t)ID_SETDATE);
    const char* what = isDate ? "SetDate" : "SetCalendarSpeed";
    const int value = (int)(int32_t)(uint32_t)rdx;
    if (caller != (isDate ? CALLER_SET_DATE : CALLER_CALENDAR_SPEED)) {
        static uint64_t seen[8] = {};
        for (int i = 0; i < 8; i++) {
            if (seen[i] == caller) break;
            if (!seen[i]) {
                seen[i] = caller;
                Log("[slice] %s(%d) from caller_rva=%llx -- not the editor, left alone (logged once per caller)\n",
                    what, value, (unsigned long long)caller);
                break;
            }
        }
        return;
    }
    // 1721426 = 0001-01-01 and 5373484 = 9999-12-31 in Julian days. The slider
    // (0x4f29f0, a value-changed handler taking a stop index) sends
    // default-ms-per-day / the stop's multiplier, and 0 for a multiplier of 0 or
    // less -- the stopped calendar -- so 0 is a real setting and must replicate;
    // a day longer than ~2.8 hours is not a stop the slider offers.
    const bool inRange = isDate ? (value >= 1721426 && value <= 5373484) : (value >= 0 && value <= 10000000);
    if (!inRange) {
        Log("[slice] %s value %d out of range -- left alone\n", what, value);
        return;
    }
    if (!SessionLive()) {
        Log("[slice] %s %d: no live session -- left alone\n", what, value);
        return;
    }
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] %s %d: no instance letter -- left alone\n", what, value); return; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] %s %d: cannot open %s -- left alone\n", what, value, p); return; }
    fprintf(f, "%s %d\n", isDate ? "SETDATE" : "CALSPEED", value);
    fclose(f);
    InterlockedExchange(&g_pendingNoCb, 1);
    InterlockedExchange(&g_pendingHonour, 0);
    InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
    Log("[slice] armed cancel: %s %d (caller_rva=%llx) -- every instance applies it at the stamp\n",
        what, value, (unsigned long long)caller);
}

static void CaptureFactory(const Factory& f, uint64_t rcx, uint64_t rdx, uint64_t r8,
                           uint64_t r9, uint64_t calleeRsp, uint64_t caller, bool cancel)
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

    // Sell / Replace / SendToDepot / SetLine. The scripting layer's wrappers (our
    // own replays on the peer) live in one block, 0xcec000..0xcf2000 (ced378 =
    // buildProposal, cee710 = SetVehicleManualDeparture, ceefae = buyVehicle);
    // anything else is the UI. ReplaceVehicle (4) is in the list: without it a
    // player's "replace with this model" reached the wire nowhere and the peer
    // kept the old vehicle.
    // 13/14 (SetColor/SetName) ship through the same writer. Leaving them out
    // of this list meant the hook CAPTURED a rename -- '[cap] SetName' is in
    // the log -- and then wrote nothing, so renaming a line looked like a
    // replication failure when it never reached the wire at all.
    if ((f.id >= 3 && f.id <= 10) || f.id == 13 || f.id == 14) {
        bool luaPath = IsScriptCaller(caller);
        if (luaPath) {
            Log("[slice] %s from the Lua path (caller=%llx) -- a replay, not shipped\n",
                f.name, (unsigned long long)caller);
            if (f.id == 7) {
                __try { ClaimLineCreateCarrier(rcx); }
                __except (EXCEPTION_EXECUTE_HANDLER) { Log("[slice] CreateLine: claim fault -- ignored\n"); }
            }
        } else {
            // UpdateLine: decode the Line FIRST. A cancel is only honest when
            // the whole new stop list is on the wire; otherwise ship the event
            // as before and let it run natively (never cancel on a failed decode).
            g_lineDecodeOk = false;
            if (f.id == 8) {
                __try { g_lineDecodeOk = DecodeLine(r9, &g_lineDecode); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_lineDecodeOk = false; }
                if (!g_lineDecodeOk && cancel) {
                    Log("[slice] UpdateLine: Line decode failed (%s) -- NOT cancelled; event ships, peers read back. "
                        "LOCKSTEP AT RISK: this edit runs on this game first and the read-back may not carry it\n", g_lineDecodeWhy);
                    cancel = false;
                }
            }
            if (f.id == 7) {
                // CreateLine: strict only when the whole create -- name, colour, line --
                // is on the wire (never cancel on a failed decode)
                __try { DecodeLineCreate(rdx, r8, st[0]); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_lcDecodeOk = false; }
                if (!g_lcDecodeOk && cancel) {
                    Log("[slice] CreateLine: decode failed -- NOT cancelled; the event ships as before\n");
                    cancel = false;
                }
            }
            bool readable = false;
            __try { readable = VehiclePayloadReadable(f.id, r8, r9, st[0]); }
            __except (EXCEPTION_EXECUTE_HANDLER) { readable = false; }
            if (!readable) {
                Log("[slice] %s: arguments not readable -- NOT cancelled, runs natively, not shipped\n", f.name);
                cancel = false;
            } else {
                const bool armed = cancel && SessionLive();
                WriteArmed(armed);
                bool shipped = false;
                __try { shipped = WriteInjectVehicleCmd(f.id, r8, r9, st[0]); }
                __except (EXCEPTION_EXECUTE_HANDLER) { Log("[slice] %s decode fault -- not shipped\n", f.name); }
                if (!shipped && cancel) {
                    // ARMED 1 is on disk with no line behind it: take it back so the
                    // next capture cannot inherit it, and let this command run.
                    if (armed) WriteArmed(false);
                    Log("[slice] %s: nothing shipped -- NOT cancelled, runs natively\n", f.name);
                    cancel = false;
                }
            }
        }
    }

    if (f.id == 2 && caller == CALLER_LUA_VEHICLE) {
        Log("[slice] VBUY from the Lua path (caller=%llx) -- a replay, not shipped\n",
            (unsigned long long)caller);
    } else if (f.id == 2) {
        // A player's buy: r9 = depot entity (value), st[0] = pointer to the
        // by-value config copy on the caller's stack. WriteArmed runs at
        // CAPTURE time, before the Add hook decides whether the cancel can be
        // honoured, so it states an INTENTION. For the buy that intention is
        // not refused: its Add fires the depot window's callback and, if the
        // fire fails, honours the armed cancel anyway (g_pendingHonour).
        // So the config must read BEFORE anything is armed: a buy cancelled
        // with no VBUY behind it is a purchase that never happens.
        bool readable = false;
        __try { readable = VehiclePayloadReadable(2, r8, r9, st[0]); }
        __except (EXCEPTION_EXECUTE_HANDLER) { readable = false; }
        if (!readable) {
            Log("[slice] BuyVehicle: config not readable -- NOT cancelled, the buy runs natively, not shipped\n");
            cancel = false;
        } else {
            const bool armed = cancel && SessionLive();
            WriteArmed(armed);
            bool shipped = false;
            __try { shipped = WriteInjectVBuy(r9, st[0]); }
            __except (EXCEPTION_EXECUTE_HANDLER) { shipped = false; }
            if (!shipped) {
                if (armed) WriteArmed(false);   // take back the ARMED 1 that has no line behind it
                Log("[slice] VBUY not shipped -- NOT cancelled, the buy runs natively\n");
                cancel = false;
            }
        }
    }

    if (cancel && SessionLive()) {
        InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
        // BuyVehicle is the one vehicle command whose UI WAITS: the depot window
        // expects the new entity back, and cancelling it fire-and-forget crashed
        // the client on an assert (2026-08-28, caller 74fda9). That is why the
        // buy was left optimistic and why it is the last host-only asymmetry in
        // a vehicle's life. So it takes the BUILD TOOL's route instead --
        // g_pendingNoCb = 0, meaning the Add hook fires the completion callback
        // before cancelling, and if it cannot fire it lets the buy run rather
        // than wedge the window. Every other vehicle/line command genuinely has
        // nothing waiting and stays fire-and-forget.
        // ReplaceVehicle (4) waits too: the vehicle window reads the
        // replacement's result entity. Its callback is a
        // heap-allocated std::function, which the Add hook now resolves
        // through the _Getimpl slot at r9+0x38 (docs/re/COMMANDS.md).
        const bool waitsForResult = (f.id == 2 || f.id == 4);
        InterlockedExchange(&g_pendingNoCb, waitsForResult ? 0 : 1);
        // CreateLine: suppressed without firing (its callback asserts on an empty
        // result) -- the Add hook MOVES the callback into the stash for our replay
        InterlockedExchange(&g_pendingStashCb, f.id == 7 ? 1 : 0);
        InterlockedExchange(&g_pendingHonour, 1);
        Log("[slice] armed cancel: %s cmd=%llx (%s)\n", f.name,
            (unsigned long long)rcx,
            waitsForResult ? "callback WILL be fired -- the depot window waits on it"
                           : "no-callback");
    } else if (cancel) {
        Log("[slice] %s: no live session -- left alone, the game handles it\n", f.name);
    }
}

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
                    bool held = false;
                    __try { held = StashLineCreateCallback(r9); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { held = false; }
                    Log(held ? "[slice] CreateLine: the line editor's callback is held for our replay at the stamp\n"
                             : "[slice] CreateLine: callback could not be held -- cancelled anyway (ARMED 1 promised the replay); the editor will not select the new line\n");
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

// ---------------------------------------------------------------------------
// TRAIN RESERVATION ORDER -- a desync the world hash cannot see.
//
// THE FINDING (RE pass on build 35924). ecs::TrainMoveSystem::Update2
// (0xabdc20) decides, once per sim step, which train gets to reserve track
// first. It builds idx[0..n-1] = iota over its train node list (0xabdfb0), then
// at 0xabe02d reads an int out of the GameTime component (Engine::GetComponent
// 0x281250 -> the accessor 0x2877c0 -> ecs::component::GameTime + 0x30), folds
// it into a std::minstd_rand (`s %= 2147483647; if (!s) s = 1` at 0xabe03d,
// then `imul r8, rax, 0xbc8f` mod 0x7fffffff at 0xabe0f0), and Fisher-Yates
// shuffles idx with it (0xabe170). The loop that follows walks the shuffled idx
// and calls transport::EdgeReservationManager::Reserve (0x21150b0) at 0xabe33a
// and 0xabe583 -- serially, on the sim thread, before the parallel movement
// loop at 0xabe7fd. First train through a junction wins it.
//
// The seed is lockstep state: same game time, same seed, same permutation. What
// is NOT lockstep state is what the permutation is applied TO. idx holds
// POSITIONS in the engine's node list, and that list is in the order the engine
// registered the train entities -- not id order, not anything the mod controls,
// and (a multi-threaded save load registers in thread-timing order) not
// necessarily the same on two peers. The mod's world hash is geometric: same
// positions, same edges, same nodes, equal hash. So two peers can hold
// identical worlds, agree on the hash, agree on the seed, and still rank the
// same two trains in opposite orders -- after which one queues two trains on a
// track the other lets through, and the divergence is real and permanent.
// Observed: a desync with no command for 1,450 game units, identical positions
// and ids, equal hash, exactly that symptom.
//
// THE PATCH. Detour 0xabe02d, replace both the seeding and the shuffle with an
// order computed from lockstep state only, and rejoin the engine at 0xabe194 --
// the two instructions that reload rsi/rcx before the reservation loop, past
// its Fisher-Yates. The order (trainorder.h, TrainOrderArrange):
//
//     rank r  = sorted by NAME (ASCII-case-insensitive, byte-wise), then id
//     jitter j = minstd_rand(GameTime+0x30) drawn in rank order, mod n/3
//     reserve in order of (r + j), ties by id
//
// so the alphabet decides who generally goes first while nobody at a busy
// junction can be starved forever -- see trainorder.h for why a strict sort was
// not enough. Every input (the set of trains, their names, the game-time seed)
// is replicated state; the node-list order is not consulted at all.
//
// READING THE NAME. There is no getter to borrow: the engine inlines
// GetComponent<Name> everywhere (58 sites; 0x45ff46 is a clean one, and the
// type_info .?AUName@component@ecs@@ it leas lives at 0x41d4c40). Measured off
// that site, a component read is three steps:
//   1. type index  = 0xd0a40(world + 0x48, &type_info)   -- a map lookup
//   2. slot        = scan world[0xa0][entity] -- a {type index, slot} list
//   3. component   = world[0x88][type index] -> +0x68 + slot * 0x20
//                    (or, for slot >= 0x40000000, the paged table at +0x80)
// and the Name component is a bare std::string: size at +0x10, capacity at
// +0x18, text inline at +0 until capacity reaches 16 -- the same MSVC layout
// this file already decodes for SetName (fid 14).
//
// STEP 2 IS DONE BY HAND ON PURPOSE. The engine's own helper for it (0xd0920)
// ends its scan with a formatted assert and an int3 when the entity has no
// component of that type: calling it for an unnamed train would not return a
// null, it would take the game down. The loop at 0xd0966 it replaces is six
// instructions, and ours stops at the end of the list instead.
//
// `world` is Update2's second argument (r13, assigned at 0xabdc6a). The
// engine's own prologue calls 0xd0a40 on r13+0x48 five times before we get
// here, so by the time the detour runs that registry has already been proved
// good by the code we are standing in.
//
// WHAT THIS ASSUMES, AND WHAT HAPPENS WHEN THE ASSUMPTION IS WRONG. Entity ids
// are NOT equal across peers by design -- replication ships positions, not ids,
// and each peer's allocator is also advanced by town growth it did on its own
// (hash-world-by-geometry-not-ids). Names are the primary key precisely because
// they ARE replicated (VNAME), but they are a weak key: trains share names, and
// every tie falls through to the id. What the tie-break needs is weaker than
// equal ids -- only that the RANK of the ids agrees, which holds while ids are
// handed out in creation order and every train is created by a replicated
// command, but not necessarily after the engine recycles a freed id
// (reused-entity-ids-hide-replayed-constructions).
//
// So the log carries `ids=`, an FNV of the entity ids in final order: equal on
// both peers is proof they are about to let the same trains through in the same
// order. The Lua side hashes the names themselves in a lane of their own
// (hash.lua, the `r:` lane), so a name that differs between peers is reported
// instead of quietly splitting the two simulations here.
//
// KILL SWITCH: `trainorder=0` in tpf2_menu_flags.txt (next to the dlls, or in
// the data dir) skips the patch entirely, leaving the engine's shuffle in
// place. Logged either way.
// ---------------------------------------------------------------------------
#include "trainorder.h"

static const uintptr_t RVA_TRAINORDER_HOOK   = 0xabe02d;   // mov rax,[rsi+0x48]
static const uintptr_t RVA_TRAINORDER_RESUME = 0xabe194;   // past the engine's shuffle
static const uintptr_t RVA_GAMETIME_GET      = 0x2877c0;   // int GameTime::get(void*) -> +0x30
static const uintptr_t RVA_ECS_TYPEINDEX     = 0x0d0a40;   // int(registry, type_info**)
static const uintptr_t RVA_TI_NAME           = 0x41d4c40;  // .?AUName@component@ecs@@
static const int       TRAINORDER_STEAL      = 16;         // 4 + 4 + 5 + 3, a clean boundary

// The 16 bytes the patch overwrites. Checked before anything is written: a
// different build must be left alone, not jmp'd into the middle of.
static const uint8_t TRAINORDER_EXPECT[TRAINORDER_STEAL] = {
    0x48, 0x8B, 0x46, 0x48,              // mov rax, [rsi+0x48]
    0x48, 0x8B, 0x48, 0x18,              // mov rcx, [rax+0x18]
    0xE8, 0x86, 0x97, 0x7C, 0xFF,        // call 0x2877c0
    0x44, 0x8B, 0xC0                     // mov r8d, eax
};
// ...and the instructions the relay resumes on, which must still be the two
// reloads the engine does on the way out of its shuffle.
static const uint8_t TRAINORDER_EXPECT_RESUME[9] = {
    0x48, 0x8B, 0x74, 0x24, 0x78,        // mov rsi, [rsp+0x78]
    0x48, 0x8B, 0x4D, 0xE8               // mov rcx, [rbp-0x18]
};

extern "C" {
    uint64_t g_trainOrderResume = 0;     // where the relay jumps when it is done
    void TrainOrderRelay();
}
static bool g_trainOrderOn = false;
static volatile LONG   g_toCalls = 0, g_toReorders = 0, g_toRefusals = 0;
static volatile LONG   g_toLastSeed = 0, g_toMaxUs = 0;
static volatile LONG64 g_toLastN = -1;

// The int the engine seeds its shuffle with, read the way the stolen bytes read
// it: rcx = *(*(this+0x48)+0x18), then the accessor. Returns 0 if the chain
// does not look like memory; TrainOrderSeedFix turns that into 1, so the order
// stays defined (and both peers get the same 0 from the same broken read).
static uint32_t TrainOrderSeed(void* self)
{
    uint8_t* s = (uint8_t*)self;
    if (!s || !Readable(s + 0x48, 8)) return 0;
    uint8_t* a = *(uint8_t**)(s + 0x48);
    if (!a || !Readable(a + 0x18, 8)) return 0;
    void* arg = *(void**)(a + 0x18);
    if (!arg) return 0;
    typedef int (*GameTimeGet)(void*);
    return (uint32_t)((GameTimeGet)(g_base + RVA_GAMETIME_GET))(arg);
}

// ---- the three steps of a component read (see the header comment) ----------

// 1. The Name component's type index in this world. Called once per sim step,
// never cached: a type index belongs to a world, and a new game or a loaded
// save is a new world at an address the old one may well have been freed from.
// One map lookup a step is not worth the risk of a stale index silently reading
// a different component type.
static int TrainOrderNameType(uint8_t* world)
{
    const void* ti = (const void*)(g_base + RVA_TI_NAME);
    typedef int (*TypeIndexFn)(void*, const void**);
    return ((TypeIndexFn)(g_base + RVA_ECS_TYPEINDEX))(world + 0x48, &ti);
}

// 2. The slot this entity's component of that type sits in, or -1 when it has
// none. Deliberately NOT 0xd0920: that one aborts the process on a miss.
static int TrainOrderSlot(uint8_t* world, int32_t entity, int typeIdx)
{
    if (entity < 0 || entity > 0x0fffffff) return -1;
    uint8_t* table = *(uint8_t**)(world + 0xa0);
    if (!table) return -1;
    uint8_t* ent = table + (size_t)entity * 24;      // vector<pair<int,int>> per entity
    uint8_t* b = *(uint8_t**)ent;
    uint8_t* e = *(uint8_t**)(ent + 8);
    if (!b || e < b || (size_t)(e - b) % 8 || (size_t)(e - b) > 8 * 4096) return -1;
    for (; b != e; b += 8) {
        int32_t t, slot;
        memcpy(&t, b, 4);
        if (t != typeIdx) continue;
        memcpy(&slot, b + 4, 4);
        return slot;
    }
    return -1;
}

// 3. Where that slot lives. Both branches of the engine's own indexing, the
// flat array and the paged table it switches to at 0x40000000.
static const uint8_t* TrainOrderComponent(uint8_t* world, int typeIdx, int slot)
{
    if (typeIdx < 0 || typeIdx > 4096 || slot < 0) return nullptr;
    uint8_t* pools = *(uint8_t**)(world + 0x88);
    if (!pools) return nullptr;
    uint8_t* pool = *(uint8_t**)(pools + (size_t)typeIdx * 8);
    if (!pool) return nullptr;
    if (slot < 0x40000000) {
        uint8_t* data = *(uint8_t**)(pool + 0x68);
        return data ? data + (size_t)slot * 0x20 : nullptr;
    }
    const int32_t e = slot - 0x40000000;
    uint8_t* pages = *(uint8_t**)(pool + 0x80);
    if (!pages) return nullptr;
    uint8_t* page = *(uint8_t**)(pages + (size_t)(e / 32) * 2 * 8);
    return page ? page + (size_t)(e % 32) * 0x20 : nullptr;
}

// The Name component is one std::string. MSVC layout, the same one the SetName
// capture in this file decodes: size at +0x10, capacity at +0x18, text inline
// at +0 while capacity is under 16, otherwise behind the pointer at +0.
static bool TrainOrderNameText(const uint8_t* comp, const char** text, uint32_t* len)
{
    uint64_t sz = 0, cap = 0;
    memcpy(&sz, comp + 0x10, 8);
    memcpy(&cap, comp + 0x18, 8);
    if (sz > TRAINORDER_NAME_MAX || cap < sz) return false;
    const char* p = (const char*)comp;
    if (cap >= 16) { uint64_t ptr = 0; memcpy(&ptr, comp, 8); p = (const char*)ptr; }
    if (!p) return false;
    *text = p; *len = (uint32_t)sz;
    return true;
}

// Is this call worth a line? Always the first one and any change in the train
// count; otherwise 1 seed in 64, picked by a hash of the seed VALUE -- which
// picks the SAME seeds on every peer whatever the game-time stride between sim
// steps is, so the two logs still line up, and keeps this off the per-step log
// budget. Carries its own "have we been here" state, so call it once per call.
static bool TrainOrderShouldLog(uint32_t seed, int64_t n)
{
    static uint32_t lastSeed = 0;
    static int64_t  lastN = -1;
    const bool first = lastN < 0;
    const bool nMoved = n != lastN;
    uint32_t h = seed * 2654435761u; h ^= h >> 16;
    const bool sample = (h >> 26) == 0;
    const bool show = first || nMoved || (seed != lastSeed && sample);
    lastSeed = seed; lastN = n;
    return show;
}

// Refusals are logged once per reason, not once per sim step.
static void TrainOrderLogRefusal(const char* why, int64_t n, uint32_t seed)
{
    static const char* lastRefused = nullptr;
    InterlockedIncrement(&g_toRefusals);
    if (why == lastRefused) return;
    lastRefused = why;
    Log("[trainorder] refused: %s (n=%lld seed=%u) -- the engine's own order stands\n",
        why, (long long)n, seed);
}

// One key per node-list position. Reused across steps so a busy world does not
// allocate once a step; the name POINTERS in it are into live components and are
// never held past the call.
//
// Update2 runs on the sim thread and its reservation loop is serial -- the
// parallel part is the movement loop further down, at 0xabe7fd -- so this buffer
// has one user. g_toBusy costs two interlocked ops a step to make sure: if that
// reading is ever wrong, the second thread backs out and says so, instead of
// resizing the vector under the first one.
static std::vector<TrainOrderKey> g_toKeys;
static volatile LONG g_toBusy = 0;

// The body of the detour. Everything it needs comes from the relay: the idx
// array the engine just filled with iota, the train count (its rbx), the
// vector's end pointer (its [rbp-0x18], kept only to cross-check the count),
// `this`, and the ECS world (its r13).
static void TrainOrderRank(int32_t* idx, int64_t n, const int32_t* idxEnd,
                           void* self, void* world, uint32_t seed)
{
    InterlockedIncrement(&g_toCalls);
    InterlockedExchange(&g_toLastSeed, (LONG)seed);
    InterlockedExchange64(&g_toLastN, (LONG64)n);

    const uint8_t* recs = nullptr;
    const char* refused = nullptr;
    if (!idxEnd || idxEnd < (const int32_t*)idx || (int64_t)(idxEnd - (const int32_t*)idx) != n)
        refused = "count does not match the index vector";
    else if (!self || !Readable((uint8_t*)self + 8, 8))
        refused = "no node list";
    else {
        // this+8 is the node-list holder; its first field is the record base.
        // (0xabe1cc: mov rdx,[rsi+8]; mov rax,[rdx]; lea r15,[rax+idx*12])
        uint8_t* holder = *(uint8_t**)((uint8_t*)self + 8);
        if (!holder || !Readable(holder, 8)) refused = "no node list";
        else {
            recs = *(const uint8_t**)holder;
            if (!recs || !Readable(recs, (size_t)n * TRAINORDER_REC)) refused = "records unreadable";
        }
    }
    if (refused) { TrainOrderLogRefusal(refused, n, seed); return; }
    if (n < 2) return;

    // Keys: the entity id always, the name when the world looks like one we can
    // read. A world we cannot read costs the names, not the ordering -- ranking
    // by id alone is still a pure function of lockstep state, and still beats
    // the registration order we are replacing.
    if ((int64_t)g_toKeys.size() < n) g_toKeys.resize((size_t)n);
    TrainOrderKey* keys = g_toKeys.data();
    for (int64_t i = 0; i < n; i++) {
        keys[i].name = nullptr; keys[i].len = 0; keys[i].score = 0;
        keys[i].id = TrainOrderRecId(recs, (int32_t)i);
    }
    uint8_t* w = (uint8_t*)world;
    const bool worldOk = w && Readable(w + 0x48, 8) && Readable(w + 0x88, 8) &&
                         Readable(w + 0xa0, 8) && *(uint8_t**)(w + 0x88) && *(uint8_t**)(w + 0xa0);
    int noName = 0;
    const int typeIdx = worldOk ? TrainOrderNameType(w) : -1;
    if (typeIdx >= 0) {
        for (int64_t i = 0; i < n; i++) {
            const int slot = TrainOrderSlot(w, keys[i].id, typeIdx);
            const uint8_t* comp = slot < 0 ? nullptr : TrainOrderComponent(w, typeIdx, slot);
            if (!comp || !TrainOrderNameText(comp, &keys[i].name, &keys[i].len)) {
                keys[i].name = nullptr; keys[i].len = 0; noName++;
            }
        }
    } else {
        noName = (int)n;
    }

    const TrainOrderOutcome o = TrainOrderArrange(idx, n, keys, seed);
    if (o.refused) { TrainOrderLogRefusal(o.refused, n, seed); return; }
    if (o.changed) InterlockedIncrement(&g_toReorders);
    if (!TrainOrderShouldLog(seed, n)) return;
    Log("[trainorder] seed=%u n=%lld named=%lld unnamed=%d reordered=%d ids=%08x%s%s\n",
        seed, (long long)n, (long long)o.named, noName, o.changed ? 1 : 0,
        TrainOrderIdHash(idx, n, recs),
        o.duplicates ? " DUPLICATE-IDS" : "",
        typeIdx >= 0 ? "" : " NO-NAMES(world unreadable)");
}

// Called by TrainOrderRelay. A fault anywhere in here costs the ordering for
// one step, never the game: the engine's array is only ever SWAPPED within
// itself, so however far the sort had got, every train is still in it exactly
// once and the reservation loop still visits each of them.
//
// The busy flag is taken and released here, around the whole body, so a fault
// cannot leave it stuck and switch the patch off for the rest of the session.
extern "C" void TrainOrderFix(int32_t* idx, int64_t n, const int32_t* idxEnd,
                              void* self, void* world)
{
    if (InterlockedCompareExchange(&g_toBusy, 1, 0) != 0) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("[trainorder] re-entered from a second thread -- this step keeps the engine's "
                "order. TrainMoveSystem::Update2 was measured as serial; it is not\n");
        }
        InterlockedIncrement(&g_toRefusals);
        return;
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    __try {
        TrainOrderRank(idx, n, idxEnd, self, world, TrainOrderSeed(self));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool told = false;
        if (!told) { told = true; Log("[trainorder] faulted -- ordering skipped this step, the game is untouched\n"); }
    }
    InterlockedExchange(&g_toBusy, 0);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0) return;
    const LONG us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    if (us > g_toMaxUs) {
        InterlockedExchange(&g_toMaxUs, us);
        // Once, and only when it actually costs something. This runs on the sim
        // thread inside the engine's own update: a millisecond here is a
        // millisecond off every sim step on every peer.
        static bool warned = false;
        if (us > 1000 && !warned) {
            warned = true;
            Log("[trainorder] SLOW: %ld us for %lld trains in one step -- over the 1 ms budget\n",
                us, (long long)g_toLastN);
        }
    }
}

// A mid-function detour with NO return path: the relay ends by jumping to
// 0xabe194 itself. InstallHook is not usable here -- it builds a trampoline out
// of the stolen bytes, and these contain a call rel32 that would then point
// into space (and we are skipping the engine's shuffle, not running it).
static bool PatchJump(uintptr_t at, void* to, int len)
{
    if (len < 14 || len > 32) return false;
    DWORD old = 0;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t patch[32];
    patch[0] = 0xFF; patch[1] = 0x25;                 // jmp [rip+0]
    memset(patch + 2, 0, 4);
    uintptr_t d = (uintptr_t)to;
    memcpy(patch + 6, &d, 8);
    memset(patch + 14, 0xCC, len - 14);
    memcpy((void*)at, patch, len);
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}

// Kill switch: `trainorder=0` in tpf2_menu_flags.txt, the same file (and the
// same dumb prefix match) the menu dll reads its own switches from. Looked up
// next to this dll first, then in the data dir.
static bool FlagsSayNoTrainOrder()
{
    for (int i = 0; i < 2; i++) {
        const char* dir = i == 0 ? g_dllDir : g_dataDir;
        if (!dir[0]) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", dir);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (!f) continue;
        char line[256]; bool off = false;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "trainorder=0", 12)) off = true;
        fclose(f);
        return off;
    }
    return false;
}

static void InstallTrainOrder()
{
    if (FlagsSayNoTrainOrder()) {
        Log("[trainorder] OFF (trainorder=0 in tpf2_menu_flags.txt) -- trains keep the "
            "engine's registration-order shuffle, which two peers can disagree about\n");
        return;
    }
    const uintptr_t at = g_base + RVA_TRAINORDER_HOOK;
    if (!Readable((const void*)at, TRAINORDER_STEAL) ||
        memcmp((const void*)at, TRAINORDER_EXPECT, TRAINORDER_STEAL) != 0) {
        char got[3 * TRAINORDER_STEAL + 1]; got[0] = 0;
        if (Readable((const void*)at, TRAINORDER_STEAL))
            for (int i = 0; i < TRAINORDER_STEAL; i++)
                snprintf(got + i * 3, 4, "%02x ", ((const uint8_t*)at)[i]);
        Log("[trainorder] NOT installed: bytes at rva=%llx are not the sequence measured "
            "on build %lu (got: %s)\n", (unsigned long long)RVA_TRAINORDER_HOOK,
            (unsigned long)GAME_BUILD_NUMBER, got[0] ? got : "unreadable");
        return;
    }
    // The call inside those bytes must really be the GameTime accessor: the
    // rel32 is build-specific, so resolve it rather than trust the byte match.
    int32_t rel = 0;
    memcpy(&rel, (const void*)(at + 9), 4);
    const uintptr_t callTarget = (uintptr_t)((int64_t)at + 13 + rel);
    if (callTarget != g_base + RVA_GAMETIME_GET) {
        Log("[trainorder] NOT installed: the call at rva=%llx resolves to %llx, not the "
            "GameTime accessor %llx\n", (unsigned long long)(RVA_TRAINORDER_HOOK + 8),
            (unsigned long long)(callTarget - g_base), (unsigned long long)RVA_GAMETIME_GET);
        return;
    }
    const uintptr_t resume = g_base + RVA_TRAINORDER_RESUME;
    if (!Readable((const void*)resume, sizeof(TRAINORDER_EXPECT_RESUME)) ||
        memcmp((const void*)resume, TRAINORDER_EXPECT_RESUME, sizeof(TRAINORDER_EXPECT_RESUME)) != 0) {
        Log("[trainorder] NOT installed: rva=%llx is not the pair of reloads the engine "
            "leaves its shuffle on\n", (unsigned long long)RVA_TRAINORDER_RESUME);
        return;
    }
    g_trainOrderResume = resume;
    if (!PatchJump(at, (void*)&TrainOrderRelay, TRAINORDER_STEAL)) {
        Log("[trainorder] NOT installed: could not write the detour at rva=%llx\n",
            (unsigned long long)RVA_TRAINORDER_HOOK);
        return;
    }
    g_trainOrderOn = true;
    Log("[trainorder] installed rva=%llx steal=%d resume=%llx -- track is reserved by "
        "name (case-insensitive) with a seeded jitter of n/%lld, never by node-list order\n",
        (unsigned long long)RVA_TRAINORDER_HOOK, TRAINORDER_STEAL,
        (unsigned long long)RVA_TRAINORDER_RESUME, (long long)TRAIN_ORDER_JITTER_DIV);
}

// ---------------------------------------------------------------------------
// A FIVE-BYTE DETOUR, AND SOMEWHERE NEAR THE EXE TO PUT IT
//
// PatchJump above writes `jmp [rip+0]` and therefore needs 14 bytes. The four
// functions below have one instruction each to spare before something that
// cannot be relocated (a call rel32) or that must run with the original rsp
// (`mov rax,rsp`), so they get the other kind of detour: a 5-byte `jmp rel32`
// into a stub allocated close enough to the exe for a rel32 to reach it, and
// the stub does the far jump. The stub page also holds the trampolines -- the
// stolen 5 bytes plus an absolute jump back to target+5 -- so a detour can hand
// the call back to the engine unchanged when it decides not to act.
//
// Why "near" is not a gamble: the exe is based at 0x140000000 and the window a
// rel32 reaches is +-2 GB around it. The search below walks the free regions
// VirtualQuery reports, up first and then down, and reserves one 4 KB page. If
// it somehow cannot, nothing is patched and the log says so -- the same failure
// mode as a byte guard that does not match.
// ---------------------------------------------------------------------------
static uint8_t* g_nearPage = nullptr;
static size_t   g_nearUsed = 0;
static const size_t NEAR_PAGE_SIZE = 4096;

static bool NearPageInit()
{
    if (g_nearPage) return true;
    if (!g_base) return false;
    const uintptr_t gran = 0x10000;
    const uintptr_t start = g_base & ~(gran - 1);
    const uintptr_t hi = g_base + 0x60000000ull;          // 1.5 GB of headroom
    const uintptr_t lo = g_base > 0x60000000ull ? g_base - 0x60000000ull : gran;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = start; a < hi; ) {
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc((void*)a, NEAR_PAGE_SIZE,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) { g_nearPage = (uint8_t*)p; return true; }
        }
        const uintptr_t next = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize + gran - 1) & ~(gran - 1);
        if (next <= a) break;
        a = next;
    }
    for (uintptr_t a = start; a > lo; a -= gran) {
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        if (mbi.State != MEM_FREE) continue;
        void* p = VirtualAlloc((void*)a, NEAR_PAGE_SIZE,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) { g_nearPage = (uint8_t*)p; return true; }
    }
    return false;
}

static uint8_t* NearAlloc(size_t n)
{
    if (!NearPageInit()) return nullptr;
    n = (n + 15) & ~(size_t)15;
    if (g_nearUsed + n > NEAR_PAGE_SIZE) return nullptr;
    uint8_t* p = g_nearPage + g_nearUsed;
    g_nearUsed += n;
    return p;
}

// Steal exactly `steal` bytes (whole instructions, no rip-relative operand, no
// call/jmp rel32 -- the caller has checked that against the measured bytes)
// and point them at `detour`. With trampolineOut, the stolen bytes are copied
// where the engine can still run them, followed by a jump to target+steal.
static bool PatchJumpNear(uintptr_t at, void* detour, int steal, void** trampolineOut)
{
    if (steal < 5 || steal > 16) return false;
    uint8_t* stub = NearAlloc(14);
    if (!stub) return false;
    uint8_t* tramp = nullptr;
    if (trampolineOut) {
        tramp = NearAlloc((size_t)steal + 14);
        if (!tramp) return false;
    }
    const int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;

    stub[0] = 0xFF; stub[1] = 0x25; memset(stub + 2, 0, 4);       // jmp [rip+0]
    const uintptr_t d = (uintptr_t)detour;
    memcpy(stub + 6, &d, 8);
    if (tramp) {
        memcpy(tramp, (const void*)at, (size_t)steal);
        tramp[steal] = 0xFF; tramp[steal + 1] = 0x25;
        memset(tramp + steal + 2, 0, 4);
        const uintptr_t back = at + (uintptr_t)steal;
        memcpy(tramp + steal + 6, &back, 8);
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)at, (SIZE_T)steal, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t patch[16];
    patch[0] = 0xE9;
    const int32_t r32 = (int32_t)rel;
    memcpy(patch + 1, &r32, 4);
    memset(patch + 5, 0xCC, (size_t)steal - 5);
    memcpy((void*)at, patch, (size_t)steal);
    VirtualProtect((void*)at, (SIZE_T)steal, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, (SIZE_T)steal);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
    if (tramp) FlushInstructionCache(GetCurrentProcess(), tramp, (SIZE_T)steal + 14);
    if (trampolineOut) *trampolineOut = tramp;
    return true;
}

// `<key>=0` in tpf2_menu_flags.txt, the same file (and the same dumb prefix
// match) the menu dll and FlagsSayNoTrainOrder read. Next to this dll first,
// then the data dir.
static bool FlagsSayOff(const char* key)
{
    const size_t klen = strlen(key);
    for (int i = 0; i < 2; i++) {
        const char* dir = i == 0 ? g_dllDir : g_dataDir;
        if (!dir[0]) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", dir);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (!f) continue;
        char line[256]; bool off = false;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, key, klen) && line[klen] == '=' && line[klen + 1] == '0') off = true;
        fclose(f);
        return off;
    }
    return false;
}

// Byte guard shared by the installers below: the bytes at `rva` must be the
// ones measured on this build, or nothing is written and the log says what was
// found instead.
static bool BytesAre(uintptr_t rva, const uint8_t* want, size_t n, const char* tag)
{
    const uintptr_t at = g_base + rva;
    if (Readable((const void*)at, n) && memcmp((const void*)at, want, n) == 0) return true;
    char got[3 * 24 + 1]; got[0] = 0;
    const size_t show = n < 24 ? n : 24;
    if (Readable((const void*)at, show))
        for (size_t i = 0; i < show; i++) snprintf(got + i * 3, 4, "%02x ", ((const uint8_t*)at)[i]);
    Log("[%s] NOT installed: bytes at rva=%llx are not the sequence measured on build %lu "
        "(got: %s)\n", tag, (unsigned long long)rva, (unsigned long)GAME_BUILD_NUMBER,
        got[0] ? got : "unreadable");
    return false;
}

// ---------------------------------------------------------------------------
// ROAD FREE SPACE -- an order-dependent float sum, three instructions from a
// junction decision.
//
// THE FINDING (RE pass on build 35924). Roads do not use the reservation
// manager that trains, ships and aircraft arbitrate with. Their one real order
// dependence is arithmetic. transport::EdgeUseManager::GetUsedSpace (0x2117350,
// and the sibling that takes a skip predicate, 0x2117140) walks one edge's
// `entries` vector, clips each vehicle's footprint to the edge, and adds the
// clipped lengths up in SINGLE precision in vector order:
//
//     0x1421173b0  loop: GetPos, clip, GetPos, clip ...
//     0x142117448  subss xmm1,xmm0        ; hi - lo
//     0x142117450  addss xmm1,xmm7        ; + the running sum
//     0x14211745a  jne 0x1421173b0
//
// `entries` order comes from EdgeUseManager::Add (0x2115f80, a push_back) and
// Remove (0x2117c50, an order-preserving erase), driven by ECS callbacks --
// that is, by the order the engine registered those vehicles in, which is not
// replicated state and which two peers can legitimately disagree about (the
// mod's world hash is geometric: same positions, same edges, equal hash). Float
// addition is not associative. Same vehicles, same footprints, different order,
// one ULP apart. And the consumer, inside the junction decision
// MotionCalculator::GetNextSpeedLimitAndVehicles (0x2213db0), is a bare
// compare:
//
//     0x142214bcd  call GetUsedSpace ; subss xmm6,xmm0       (space left)
//     0x142214e76  comiss xmm12,xmm6 ; jbe -> BrakePoint      ("not enough space")
//
// so that ULP decides whether a bus enters the junction or stops at it. One
// peer goes, the other waits, and nothing in the command stream or the hash
// ever mentions it.
//
// THE PATCH. Both functions are replaced at their first instruction with the
// same algorithm -- same clipping, same GetPos calls per entry in the same
// order, same per-term rounding -- that collects the terms, SORTS them, and
// sums them in double (roadspace.h, which explains why sorting and not just
// widening). The engine's own body stays reachable behind a 5-byte trampoline
// and is what runs whenever the detour meets a shape it does not recognise --
// a bad entry span, more than ROADSPACE_MAX_TERMS vehicles on one edge, a
// predicate object with no target, or a fault while walking -- so a surprise
// costs the determinism for that call, never the answer. The fallback is
// logged once.
//
// THE ABIs, measured off the two prologues and their callers (0x142214bcd,
// 0x140ab5c33, 0x142116845):
//
//   float GetUsedSpace(const EdgeId& id) const
//       rcx = this, rdx = &id. GetEdgeDataPtr (0x2116330; rcx = this,
//       rdx = &id) returns the EdgeData* or null, and null is 0.0f.
//       EdgeData: +0 float length, +8/+0x10/+0x18 the entries vector.
//       GetPos (0x21163c0; rcx = this, edx = entry.comp, xmm2 = length)
//       returns the vehicle's position along the edge in xmm0. Pure: it reads
//       the MovePath component and never writes.
//   float GetUsedSpace(const EdgeId& id,
//                      const std::function<bool(const Entry&, float&)>& skip) const
//       rcx = this, rdx = &id, r8 = &skip. The bool at id+8 picks the
//       direction: only entries whose own `forward` byte matches are counted
//       (loop A at 0x1421171a1 for a set flag, loop B at 0x142117280 for a
//       clear one). `skip` is an MSVC std::function: _Getimpl() is the last
//       pointer of the 64-byte object, at +0x38 (0x1421171cc `mov rcx,
//       [r14+0x38]`), a null one goes to _Xbad_function_call (0x142117344 ->
//       0x2bf61ca), and the call is virtual slot 2, _Do_call, at vtable+0x10
//       (0x1421171ea `call [rax+0x10]`), taking rdx = the entry and r8 = the
//       address of a float the engine then discards. A null target is handed
//       straight back to the engine, so that throw still happens exactly when
//       it did.
//
// COST. No VirtualQuery on this path: GetUsedSpace runs once per road vehicle
// per look-ahead edge per step, so every check here is arithmetic on the
// pointers the engine itself is about to dereference, and a fault is caught by
// the SEH frame and handed to the engine's own body (which then does exactly
// what it would have done today).
//
// KILL SWITCH: `roadspace=0` in tpf2_menu_flags.txt leaves both functions alone.
// ---------------------------------------------------------------------------
#include "roadspace.h"

static const uintptr_t RVA_ROADSPACE_A   = 0x2117350;   // GetUsedSpace(EdgeId)
static const uintptr_t RVA_ROADSPACE_B   = 0x2117140;   // ...with a skip predicate
static const uintptr_t RVA_EDGEUSE_DATA  = 0x2116330;   // GetEdgeDataPtr(this, EdgeId*)
static const uintptr_t RVA_EDGEUSE_POS   = 0x21163c0;   // GetPos(this, compIdx, length)
static const int       ROADSPACE_STEAL   = 5;           // one whole `mov [rsp+d8],reg`

// The prologues, checked well past the five bytes actually stolen: a build that
// matches 18 bytes here is the build these RVAs were measured on.
static const uint8_t ROADSPACE_EXPECT_A[18] = {
    0x48, 0x89, 0x6C, 0x24, 0x20,        // mov [rsp+0x20], rbp   <- the 5 stolen
    0x56,                                // push rsi
    0x48, 0x83, 0xEC, 0x40,              // sub rsp, 0x40
    0x48, 0x8B, 0xE9,                    // mov rbp, rcx
    0xE8, 0xCE, 0xEF, 0xFF, 0xFF         // call 0x2116330
};
static const uint8_t ROADSPACE_EXPECT_B[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,        // mov [rsp+0x18], rbx   <- the 5 stolen
    0x56, 0x57, 0x41, 0x56,              // push rsi / rdi / r14
    0x48, 0x83, 0xEC, 0x40,              // sub rsp, 0x40
    0x4D, 0x8B, 0xF0                     // mov r14, r8
};
// ...and the two helpers the replacement calls, so a byte match that landed on
// some other function cannot pass.
static const uint8_t EDGEUSE_DATA_EXPECT[7] = {
    0x48, 0x83, 0xEC, 0x28,              // sub rsp, 0x28
    0x4C, 0x63, 0x02                     // movsxd r8, dword ptr [rdx]
};
static const uint8_t EDGEUSE_POS_EXPECT[8] = {
    0x48, 0x83, 0xEC, 0x28,              // sub rsp, 0x28
    0x85, 0xD2,                          // test edx, edx
    0x78, 0x6C                           // js  (negative component index)
};

typedef const uint8_t* (*EdgeDataFn)(void*, const void*);
typedef float (*EdgePosFn)(void*, int, float);
typedef float (*RoadSpaceAFn)(void*, const void*);
typedef float (*RoadSpaceBFn)(void*, const void*, void*);

static bool         g_rsOn = false;
static RoadSpaceAFn g_rsOrigA = nullptr;
static RoadSpaceBFn g_rsOrigB = nullptr;
static volatile LONG g_rsCallsA = 0, g_rsCallsB = 0, g_rsDiffs = 0;
static volatile LONG g_rsMaxN = 0, g_rsHanded = 0, g_rsFaults = 0;

static int RoadSpaceSehFilter(unsigned code)
{
    // Only a bad read. A C++ exception from the engine's own skip predicate
    // (0xE06D7363) must keep unwinding through this frame, not be swallowed
    // here and turned into a number.
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                              : EXCEPTION_CONTINUE_SEARCH;
}

// Walk one edge's entries and collect the per-entry terms. Returns false when
// the shape is not the one measured, in which case nothing with a side effect
// has been called (GetEdgeDataPtr and GetPos are pure reads) and the caller
// hands the whole thing back to the engine. `*empty` means the engine's own
// answer would have been 0.0f (no edge data, or no entries).
static bool RoadSpaceCollect(void* self, const void* edgeId, void* fnObj,
                             RoadSpaceAcc* acc, bool* empty)
{
    const EdgeDataFn getData = (EdgeDataFn)(g_base + RVA_EDGEUSE_DATA);
    const EdgePosFn  getPos  = (EdgePosFn)(g_base + RVA_EDGEUSE_POS);
    RoadSpaceBegin(acc);
    *empty = true;
    if (!self || !edgeId) return false;

    // The filtered overload reads the direction out of the EdgeId before it
    // touches anything else (0x142117174 `cmp byte ptr [rbx+8],0`), and the
    // predicate's target pointer before it calls anything.
    bool wantForward = false;
    uint8_t* impl = nullptr;
    if (fnObj) {
        wantForward = *((const uint8_t*)edgeId + 8) != 0;
        impl = *(uint8_t**)((const uint8_t*)fnObj + 0x38);
        if (!impl) return false;                        // null -> the engine throws
    }

    const uint8_t* ed = getData(self, edgeId);
    if (!ed) return true;                               // engine returns 0.0f

    const RoadUseEntry* b = *(const RoadUseEntry* const*)(ed + 8);
    const RoadUseEntry* e = *(const RoadUseEntry* const*)(ed + 0x10);
    if (b == e) return true;                            // no entries: 0.0f
    if (!b || !e || e < b) return false;
    const size_t span = (size_t)((const uint8_t*)e - (const uint8_t*)b);
    if (span % sizeof(RoadUseEntry)) return false;
    if (span / sizeof(RoadUseEntry) > (size_t)ROADSPACE_MAX_TERMS) return false;
    *empty = false;

    typedef bool (*SkipFn)(void*, const void*, float*);
    for (const RoadUseEntry* p = b; p != e; p++) {
        const bool fwd = p->forward != 0;
        if (fnObj) {
            if (fwd != wantForward) continue;
            // The engine reads the edge length fresh for every GetPos call
            // (`movss xmm2,[rsi]`), so this does too: the predicate is engine
            // code and may have moved it.
            float len0; memcpy(&len0, ed, 4);
            const float pos0 = getPos(self, p->comp, len0);
            float v = fwd ? (pos0 + p->boundsFront) : (len0 - (pos0 - p->boundsFront));
            const SkipFn skip = *(SkipFn*)(*(uint8_t**)impl + 0x10);
            if (skip(impl, p, &v)) continue;
        }
        float len1; memcpy(&len1, ed, 4);
        const float pos1 = getPos(self, p->comp, len1);
        float len2; memcpy(&len2, ed, 4);
        const float pos2 = getPos(self, p->comp, len2);
        if (fwd) {
            float len3; memcpy(&len3, ed, 4);
            RoadSpaceAdd(acc, RoadSpaceTermForward(pos1, pos2, p->boundsBack,
                                                   p->boundsFront, len3));
        } else {
            RoadSpaceAdd(acc, RoadSpaceTermBackward(pos1, pos2, p->boundsBack,
                                                    p->boundsFront));
        }
    }
    return true;
}

// Counters for the alive line, and the two lines that are the live proof of
// the finding: the busiest edge seen, and the first time the ordered sum is a
// different float from the one the engine would have returned.
static void RoadSpaceNote(const RoadSpaceAcc* acc, float out, bool filtered)
{
    InterlockedIncrement(filtered ? &g_rsCallsB : &g_rsCallsA);
    if ((LONG)acc->total > g_rsMaxN) InterlockedExchange(&g_rsMaxN, (LONG)acc->total);
    if (acc->n >= 2 && memcmp(&out, &acc->asEngine, sizeof(float)) != 0) {
        InterlockedIncrement(&g_rsDiffs);
        static bool told = false;
        if (!told) {
            told = true;
            Log("[roadspace] first answer changed: %d vehicles on one edge (%s overload), "
                "engine %.9g -> ordered %.9g (delta %.3g) -- this is the ULP that used to "
                "decide a junction\n",
                acc->n, filtered ? "filtered" : "plain", (double)acc->asEngine, (double)out,
                (double)out - (double)acc->asEngine);
        }
    }
}

// Every path that gives the call back to the engine's own body. Logged once.
static void RoadSpaceHanded(const char* why, bool filtered)
{
    InterlockedIncrement(&g_rsHanded);
    static bool said = false;
    if (said) return;
    said = true;
    Log("[roadspace] handed back to the engine (%s overload): %s -- that call's sum "
        "was the engine's own, in vector order. Counted on the alive line from now on\n",
        filtered ? "filtered" : "plain", why);
}

extern "C" float RoadSpaceDetourA(void* self, const void* edgeId)
{
    RoadSpaceAcc acc;
    bool empty = true, ok = false;
    __try {
        ok = RoadSpaceCollect(self, edgeId, nullptr, &acc, &empty);
    } __except (RoadSpaceSehFilter(GetExceptionCode())) {
        InterlockedIncrement(&g_rsFaults);
        RoadSpaceHanded("fault while walking the entries", false);
        return g_rsOrigA(self, edgeId);
    }
    if (!ok) {
        RoadSpaceHanded("entry span, count or edge id not the measured shape", false);
        return g_rsOrigA(self, edgeId);
    }
    if (empty) return 0.0f;
    const float out = RoadSpaceResult(&acc);
    RoadSpaceNote(&acc, out, false);
    return out;
}

extern "C" float RoadSpaceDetourB(void* self, const void* edgeId, void* fnObj)
{
    RoadSpaceAcc acc;
    bool empty = true, ok = false;
    __try {
        ok = RoadSpaceCollect(self, edgeId, fnObj, &acc, &empty);
    } __except (RoadSpaceSehFilter(GetExceptionCode())) {
        InterlockedIncrement(&g_rsFaults);
        RoadSpaceHanded("fault while walking the entries", true);
        return g_rsOrigB(self, edgeId, fnObj);
    }
    if (!ok) {
        RoadSpaceHanded("entry span, count, edge id or predicate not the measured shape", true);
        return g_rsOrigB(self, edgeId, fnObj);
    }
    if (empty) return 0.0f;
    const float out = RoadSpaceResult(&acc);
    RoadSpaceNote(&acc, out, true);
    return out;
}

// ---------------------------------------------------------------------------
// ROAD ENTRY ORDER -- the per-edge `entries` vector in a canonical order.
//
// THE FINDING (RE, 2026-09-16, after a hot join drifted buses ~300 m at equal
// sim time while the world hash stayed locked). EdgeUseManager's per-edge
// `entries` are not serialized: they are rebuilt on load (EntityAdded 0xa64be0
// -> AddToEdgeUseManager 0xa64390 -> Add 0x2115f80, a push_back) in the order
// the loaded save registers vehicles, while the host's vector holds the order
// they ARRIVED on the edge over the whole game. Same vehicles, different order.
// GetUsedSpace's float sum over that order is already neutralised (ROAD FREE
// SPACE above). The other order-sensitive consumer is the lead-vehicle search
// in GetNext (0x2116990/0x2116e10/0x2116160/0x21164d0): an exact-float min
// search that keeps the FIRST entry on a tie, so two vehicles at bit-identical
// positions (queued at a stop) pick a different leader on the two peers, the
// brake point differs, and car-following amplifies it down the line.
//
// THE PATCH. A post-call hook right after AddToEdgeUseManager's call to Add
// (0xa64473, the 8-byte `mov rbx,[rsp+0x80]` that follows it) re-sorts that
// edge's entries by vehicle NAME (the train-order rule: ASCII-case-insensitive
// byte-wise, then entity id), which is replicated state on every peer. Add is
// the only writer that appends (Remove is an order-preserving erase), so the
// vector is canonical after every mutation. At the hook, rsi is still the
// EdgeUseManager, rbx the ecs world (AddToEdgeUseManager uses it for the type
// index map at +0x48) and the EdgeId Add was given is still at [rsp+0x30]. The
// EdgeData is found the engine's way (GetEdgeDataPtr 0x2116330), and a vector
// we cannot make sense of (bad span, more than ROADENTRIES_MAX vehicles on one
// edge, a fault) is left exactly as the engine built it and counted.
//
// Kill switch: roadentries=0 in tpf2_menu_flags.txt.
static const uintptr_t RVA_ROADENTRIES_HOOK = 0xa64473;   // right after `call 0x2115f80` in AddToEdgeUseManager
static const uint8_t ROADENTRIES_EXPECT[8] = { 0x48, 0x8B, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00 };   // mov rbx,[rsp+0x80]
static const uint32_t ROADENTRIES_EDGEID_OFF = 0x30;      // [rsp+0x30] at the hook = the EdgeId passed to Add
static const int      ROADENTRIES_MAX = 512;
static bool g_reOn = false;
static volatile LONG g_reCalls = 0, g_reSorted = 0, g_reRefused = 0, g_reFaults = 0, g_reMaxN = 0, g_reShown = 0;

struct RoadEntryKey { TrainOrderKey k; int32_t pos; };

static bool RoadEntriesLess(const RoadEntryKey& a, const RoadEntryKey& b)
{
    const int c = TrainOrderNameCmp(a.k, b.k);
    if (c) return c < 0;
    return a.k.id < b.k.id;
}

static void RoadEntriesSortImpl(uint8_t* world, uint8_t* mgr, const void* edgeId)
{
    typedef uint8_t* (*GetEdgeData)(void*, const void*);
    uint8_t* ed = ((GetEdgeData)(g_base + RVA_EDGEUSE_DATA))(mgr, edgeId);
    if (!ed || !Readable(ed, 0x20)) return;
    uint8_t* begin = *(uint8_t**)(ed + 8);
    uint8_t* end = *(uint8_t**)(ed + 0x10);
    if (!begin || end < begin || (size_t)(end - begin) % sizeof(RoadUseEntry)) { InterlockedIncrement(&g_reRefused); return; }
    const int64_t n = (int64_t)((end - begin) / sizeof(RoadUseEntry));
    if (n > g_reMaxN) g_reMaxN = (LONG)n;
    if (n < 2) return;
    if (n > ROADENTRIES_MAX || !Readable(begin, (size_t)(end - begin))) { InterlockedIncrement(&g_reRefused); return; }
    RoadEntryKey keys[ROADENTRIES_MAX];
    RoadUseEntry rec[ROADENTRIES_MAX];
    memcpy(rec, begin, (size_t)n * sizeof(RoadUseEntry));
    const int typeIdx = TrainOrderNameType(world);
    for (int64_t i = 0; i < n; i++) {
        keys[i].k.name = ""; keys[i].k.len = 0; keys[i].k.id = rec[i].entity; keys[i].pos = (int32_t)i;
        if (typeIdx >= 0) {
            const int slot = TrainOrderSlot(world, rec[i].entity, typeIdx);
            const uint8_t* comp = slot >= 0 ? TrainOrderComponent(world, typeIdx, slot) : nullptr;
            if (comp) TrainOrderNameText(comp, &keys[i].k.name, &keys[i].k.len);
        }
    }
    // insertion sort (n is a handful of vehicles per edge; stable, no allocation)
    for (int64_t i = 1; i < n; i++) {
        RoadEntryKey t = keys[i];
        int64_t j = i - 1;
        while (j >= 0 && RoadEntriesLess(t, keys[j])) { keys[j + 1] = keys[j]; j--; }
        keys[j + 1] = t;
    }
    bool changed = false;
    for (int64_t i = 0; i < n; i++) if (keys[i].pos != i) { changed = true; break; }
    if (!changed) return;
    for (int64_t i = 0; i < n; i++) memcpy(begin + i * sizeof(RoadUseEntry), &rec[keys[i].pos], sizeof(RoadUseEntry));
    InterlockedIncrement(&g_reSorted);
    if (InterlockedIncrement(&g_reShown) <= 4)
        Log("[roadentries] edge with %lld vehicles re-ordered by name (first now entity %d)\n", (long long)n, rec[keys[0].pos].entity);
}

extern "C" void RoadEntriesSort(uint8_t* world, uint8_t* mgr, const void* edgeId)
{
    InterlockedIncrement(&g_reCalls);
    __try { if (world && mgr && edgeId) RoadEntriesSortImpl(world, mgr, edgeId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_reFaults); }
}

static void InstallRoadEntries()
{
    if (FlagsSayOff("roadentries")) {
        Log("[roadentries] OFF (roadentries=0 in tpf2_menu_flags.txt) -- a road edge's vehicle "
            "entries keep the engine's arrival/load order\n");
        return;
    }
    if (!BytesAre(RVA_ROADENTRIES_HOOK, ROADENTRIES_EXPECT, sizeof(ROADENTRIES_EXPECT), "roadentries")) return;
    // the call right before the hook must be EdgeUseManager::Add
    {
        uint8_t pre[5] = { 0 };
        memcpy(pre, (const void*)(g_base + RVA_ROADENTRIES_HOOK - 5), 5);
        int32_t rel = 0; memcpy(&rel, pre + 1, 4);
        if (pre[0] != 0xE8 || (uintptr_t)((int64_t)(RVA_ROADENTRIES_HOOK - 5) + 5 + rel) != 0x2115f80) {
            Log("[roadentries] NOT installed: the call before rva=%llx is not EdgeUseManager::Add\n", (unsigned long long)RVA_ROADENTRIES_HOOK);
            return;
        }
    }
    uint8_t* stub = NearAlloc(96);
    if (!stub) { Log("[roadentries] NOT installed: no page for the stub\n"); return; }
    const uintptr_t helper = (uintptr_t)&RoadEntriesSort;
    size_t k = 0;
    // r8 = &EdgeId (rsp+0x30 at the hook, BEFORE anything is pushed), rcx = world (rbx), rdx = manager (rsi)
    stub[k++] = 0x4C; stub[k++] = 0x8D; stub[k++] = 0x44; stub[k++] = 0x24; stub[k++] = (uint8_t)ROADENTRIES_EDGEID_OFF; // lea r8,[rsp+0x30]
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xCB;                    // mov rcx, rbx
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xD6;                    // mov rdx, rsi
    stub[k++] = 0x55;                                                        // push rbp
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xEC;                    // mov rbp, rsp
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xE4; stub[k++] = 0xF0;  // and rsp, -16
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xEC; stub[k++] = 0x20;  // sub rsp, 0x20
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &helper, 8); k += 8;// mov rax, RoadEntriesSort
    stub[k++] = 0xFF; stub[k++] = 0xD0;                                      // call rax
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xE5;                    // mov rsp, rbp
    stub[k++] = 0x5D;                                                        // pop rbp
    memcpy(stub + k, ROADENTRIES_EXPECT, sizeof(ROADENTRIES_EXPECT)); k += sizeof(ROADENTRIES_EXPECT);   // mov rbx,[rsp+0x80] (stolen)
    const uintptr_t resume = g_base + RVA_ROADENTRIES_HOOK + sizeof(ROADENTRIES_EXPECT);
    stub[k++] = 0xE9;
    const int32_t rel = (int32_t)((int64_t)resume - (int64_t)((uintptr_t)stub + k + 4));
    memcpy(stub + k, &rel, 4); k += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, k);
    const uintptr_t at = g_base + RVA_ROADENTRIES_HOOK;
    const int64_t nrel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) { Log("[roadentries] NOT installed: stub out of reach\n"); return; }
    DWORD old = 0;
    if (!VirtualProtect((void*)at, 8, PAGE_EXECUTE_READWRITE, &old)) { Log("[roadentries] NOT installed: could not unprotect\n"); return; }
    uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    const int32_t r32 = (int32_t)nrel;
    memcpy(patch + 1, &r32, 4);
    memcpy((void*)at, patch, 8);
    VirtualProtect((void*)at, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 8);
    g_reOn = true;
    Log("[roadentries] installed: a road edge's vehicle entries are kept in name order on every peer (hook at rva=%llx)\n",
        (unsigned long long)RVA_ROADENTRIES_HOOK);
}

static void InstallRoadSpace()
{
    if (FlagsSayOff("roadspace")) {
        Log("[roadspace] OFF (roadspace=0 in tpf2_menu_flags.txt) -- road free space keeps "
            "the engine's single-precision sum, which depends on vector order\n");
        return;
    }
    if (!BytesAre(RVA_ROADSPACE_A, ROADSPACE_EXPECT_A, sizeof(ROADSPACE_EXPECT_A), "roadspace")) return;
    if (!BytesAre(RVA_ROADSPACE_B, ROADSPACE_EXPECT_B, sizeof(ROADSPACE_EXPECT_B), "roadspace")) return;
    if (!BytesAre(RVA_EDGEUSE_DATA, EDGEUSE_DATA_EXPECT, sizeof(EDGEUSE_DATA_EXPECT), "roadspace")) return;
    if (!BytesAre(RVA_EDGEUSE_POS, EDGEUSE_POS_EXPECT, sizeof(EDGEUSE_POS_EXPECT), "roadspace")) return;
    // The call inside the first prologue must really be GetEdgeDataPtr: the
    // rel32 is build-specific, so resolve it rather than trust the byte match.
    int32_t rel = 0;
    memcpy(&rel, ROADSPACE_EXPECT_A + 14, 4);
    const uintptr_t target = (uintptr_t)((int64_t)RVA_ROADSPACE_A + 18 + rel);
    if (target != RVA_EDGEUSE_DATA) {
        Log("[roadspace] NOT installed: the call at rva=%llx resolves to %llx, not the edge "
            "data lookup %llx\n", (unsigned long long)(RVA_ROADSPACE_A + 13),
            (unsigned long long)target, (unsigned long long)RVA_EDGEUSE_DATA);
        return;
    }
    void* trampA = nullptr;
    void* trampB = nullptr;
    if (!PatchJumpNear(g_base + RVA_ROADSPACE_A, (void*)&RoadSpaceDetourA, ROADSPACE_STEAL, &trampA)) {
        Log("[roadspace] NOT installed: could not write the detour at rva=%llx (no page within "
            "reach of a rel32?)\n", (unsigned long long)RVA_ROADSPACE_A);
        return;
    }
    g_rsOrigA = (RoadSpaceAFn)trampA;
    g_rsOn = true;
    if (!PatchJumpNear(g_base + RVA_ROADSPACE_B, (void*)&RoadSpaceDetourB, ROADSPACE_STEAL, &trampB)) {
        // The first one is in and working; the sibling is the rarer path.
        Log("[roadspace] PARTIAL: GetUsedSpace is ordered, the filtered sibling at rva=%llx is "
            "not -- its sum still depends on vector order\n", (unsigned long long)RVA_ROADSPACE_B);
        return;
    }
    g_rsOrigB = (RoadSpaceBFn)trampB;
    Log("[roadspace] installed rva=%llx,%llx steal=%d cap=%d -- free space on an edge is "
        "summed in ascending order in double, so every peer gets the same float\n",
        (unsigned long long)RVA_ROADSPACE_A, (unsigned long long)RVA_ROADSPACE_B,
        ROADSPACE_STEAL, ROADSPACE_MAX_TERMS);
}

// ---------------------------------------------------------------------------
// SHARED STATIONS -- one comparison in the line editor is the whole gate.
//
// THE FINDING (RE pass on build 35924, 2026-09-16). In companies mode a player
// clicking ANOTHER company's station in the line editor got nothing: no stop,
// no message, no sound. Walking the click path from the top:
//
//   UI::LineEditor's "add station" tool is a UI::_anon_46B7A670::StationSelector
//   (vftable 0x3010860) paired with a UI::_anon_46B7A670::StationFilter
//   (vftable 0x3010848). Both are built together in the line editor's creation
//   chain, 0x5fc5bc and 0x5fc83a inside 0x5fc560. The selector asks the filter
//   whether an entity under the cursor may be reported at all (UI::IFilter slot
//   1, through the accept helper at 0x439ea0), so a filter that says no makes
//   the entity invisible to the tool: the click lambdas (0x60bea0, which adds
//   the stop, and 0x60c040, the cursor hint) never see it. THAT is why the
//   rejection is silent -- neither of those two has an owner test of its own.
//
//   StationFilter::IsValid is 0x6095d0. It calls
//   AddStationInputComponentChecker::IsValidInput (0x609b00, funcsig-named),
//   which classifies what was clicked into
//   UI::`anonymous namespace'::ValidAddStationSelection and pairs it with the
//   entity to add: Station -> its station group (0), StationGroup (0), a track
//   or road edge the line may put a waypoint on (1), Town (2), depot/other (3),
//   nothing usable (4). IsValidInput itself has NO owner test -- the earlier
//   note in docs/SHARED_INFRA.md was right about that function and wrong about
//   where to look. The owner test is in its CALLER:
//
//     0x609610  lea  rcx, [rbx+8]
//     0x609614  call 0x8b9e60            ; the engine, off the filter
//     0x609619  lea  rdx, [rsp+0x34]     ; the entity IsValidInput paired
//     0x60961e  mov  rcx, rax
//     0x609621  call 0x472900            ; GetComponentPtr<component::PlayerOwned>
//     0x609626  test rax, rax
//     0x609629  je   0x609605            ; no owner    -> accept
//     0x60962b  mov  eax, [rax]          ; PlayerOwned.player
//     0x60962d  test eax, eax
//     0x60962f  js   0x609605            ; owner < 0   -> accept
//     0x609631  cmp  eax, [rbx+0x28]     ; <-- THE GATE
//     0x609634  je   0x609605            ; same owner  -> accept
//     0x609636  xor  eax, eax            ; another company -> REJECT, silently
//
//   and it runs only for classes 0 and 1 -- exactly the station, station group
//   and line-usable edge cases. Classes 2, 3 and 4 skip it. So the comparison
//   this patch answers can only ever be about a stop the line editor was about
//   to accept, never about anything else.
//
//   StationFilter+0x28 is the local human player entity. It is threaded from
//   UI::CGameUI::CreateUI (0x56a121 `call 0x8bb7f0` -- the view manager's game
//   state -- then `mov edi,[rax+0x214]`) through the line list (0x613340) and
//   the LineEditor constructor (0x5fa970, argument 15 at [rbp+0x1f0]) into the
//   filter (0x5fc857 `mov [rcx+0x28], eax`). GameState+0x214 is the same field
//   two other UI owner gates compare against -- 0x8b3020 `cmp *PlayerOwned,
//   [gameState+0x214]` and 0x8a3c20 -- and the one GameState::Replicate copies.
//
// EVERYTHING DOWNSTREAM WAS WALKED AND HAS NO SUCH TEST. The complete set of
// PlayerOwned readers in the binary is 43 inlined type-descriptor sites plus
// the callers of the two accessors (0x472900, 0xc5e20); none of the ones on
// this path compares two entities' owners:
//   make_cmd::UpdateLine 0x9df4e0 and its sim-thread handler 0x9d9fd0 (variant
//   tag 5) assert only `lineEntity != ecs::Entity()` and write the new Line
//   component -- a foreign station id in a replayed updateLine is NOT stripped;
//   ecs::LineSystem::EntityAdded 0xa43400 asserts only that each stop's station
//   group exists; line_util::GetBestLineAssignment 0x215d660,
//   line_util::CalcSectionPaths 0x215a050, CalcLineStopTerminal 0x96f3b0,
//   FindNextFreeTerminal 0xad40b0, ecs::ComputeTerminalConnectivity 0xa42540,
//   station_util::GetCarriers 0x218d720 and GetTerminalPersonEdges 0x218f370,
//   and the person-side LinesExpander (AddStation 0x977410, VisitLines
//   0x977640) contain no PlayerOwned read at all.
// The owner reads that do exist on neighbouring paths are all "charge or
// attribute to the vehicle's own company" (TransportVehicleSystem::
// ChargeRunningCosts 0xad11d0, HandleVehicleArrived 0xad5f70) or UI "is this
// mine" display gates (UI::GetEntitiesForPlayer 0x73d8d0, the entity window
// 0x8b3020 and 0x8a3c20). ONE genuine gate is left deliberately alone:
// vehicle_util::common::FindPathToDepot's search (the compare at 0x216fa52)
// requires the depot's PlayerOwned to equal the vehicle's, so B's vehicles
// still only ever service in B's own depots.
//
// THE PATCH. Five bytes at 0x609631 -- the `cmp` and the `je`, two whole
// instructions, nothing branches into them -- become a jump into a stub that
// asks a helper and then jumps to the engine's own accept (0x609605) or reject
// (0x609636) label. The helper answers "same owner" for a foreign owner ONLY
// while companies mode is live (line 1 of mp_company_cfg.txt, the file the menu
// dll writes at session start and companies.lua reads); otherwise it replays
// the engine's comparison exactly. Outside companies mode there is only one
// player entity, so that comparison cannot fail and the patch is a no-op --
// the mode check is belt and braces, not the safety.
//
// At 0x609631 the function has done `push rbx; sub rsp,0x20`, so rsp is
// 16-aligned and a call from the stub is ABI-correct; rbx is the filter and is
// preserved; eax is dead on both branch targets (they set it themselves), and
// no other register is read after this point, so the stub may clobber rax, rcx
// and rdx freely.
//
// WHAT THIS DOES NOT DO. Ownership does not change: the station stays A's, the
// mod still refuses B's edits and demolition (shared_infra.lua cmMayModify),
// station maintenance stays on A's books and the line's income stays on B's.
//
// KILL SWITCH: `sharedstations=0` in tpf2_menu_flags.txt leaves the comparison
// alone and foreign stations stay unusable.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_SHAREDSTATIONS_GUARD  = 0x609610;  // start of the owner test
static const uintptr_t RVA_SHAREDSTATIONS_SITE   = 0x609631;  // cmp eax,[rbx+0x28]
static const uintptr_t RVA_SHAREDSTATIONS_ACCEPT = 0x609605;  // mov eax,1; ret
static const uintptr_t RVA_SHAREDSTATIONS_REJECT = 0x609636;  // xor eax,eax; ret
static const uintptr_t RVA_SS_GETENGINE          = 0x8b9e60;  // the filter's engine accessor
static const uintptr_t RVA_SS_GETPLAYEROWNED     = 0x472900;  // GetComponentPtr<PlayerOwned>
static const int       SHAREDSTATIONS_STEAL      = 5;         // cmp (3) + je (2)

// The whole test, from the engine lookup to the two labels the stub jumps to.
// A build that matches all 46 bytes is the build these RVAs were measured on.
static const uint8_t SHAREDSTATIONS_EXPECT[46] = {
    0x48, 0x8D, 0x4B, 0x08,              // lea  rcx, [rbx+8]
    0xE8, 0x47, 0x08, 0x2B, 0x00,        // call 0x8b9e60
    0x48, 0x8D, 0x54, 0x24, 0x34,        // lea  rdx, [rsp+0x34]
    0x48, 0x8B, 0xC8,                    // mov  rcx, rax
    0xE8, 0xDA, 0x92, 0xE6, 0xFF,        // call 0x472900
    0x48, 0x85, 0xC0,                    // test rax, rax
    0x74, 0xDA,                          // je   0x609605
    0x8B, 0x00,                          // mov  eax, [rax]
    0x85, 0xC0,                          // test eax, eax
    0x78, 0xD4,                          // js   0x609605
    0x3B, 0x43, 0x28,                    // cmp  eax, [rbx+0x28]   <- the 5 stolen
    0x74, 0xCF,                          // je   0x609605
    0x33, 0xC0,                          // xor  eax, eax
    0x48, 0x83, 0xC4, 0x20,              // add  rsp, 0x20
    0x5B,                                // pop  rbx
    0xC3                                 // ret
};

static bool  g_ssOn = false;
static long  g_ssCalls = 0;     // foreign owners this filter was asked about
static long  g_ssOpened = 0;    // ...of which were let through
static long  g_ssRefused = 0;   // ...refused by the station permissions (mp_company_perms.txt)
static bool  g_ssSaidOnce = false;

// Companies mode, from the file both sides already share: line 1 of
// mp_company_cfg.txt is "companies" or "coop" (menu_hook.cpp writeCompanyCfg,
// companies.lua cmReadConfig). Cached, because the filter runs on hover.
// A stale answer is harmless: outside companies mode the comparison it guards
// cannot fail anyway.
// Companies mode is live when the SIM says so: `cm=companies` on the mod's
// status line (lockstep_status_<letter>.txt, written every 15 ticks, the file
// SessionLive already reads). The lobby's mp_company_cfg.txt only says what
// the roster was at START; a company created in game (CMNEW) never reaches
// it, so the gate read "coop" on both machines and opened nothing while being
// asked 2,344 times (2026-09-16). The file is still honoured as a second yes.
static bool SharedStationsCompaniesLive()
{
    static ULONGLONG last = 0;
    static bool cached = false;
    const ULONGLONG now = GetTickCount64();
    if (last && now - last < 2000) return cached;
    last = now;
    cached = false;
    if (!g_dataDir[0]) return cached;
    char p[MAX_PATH];
    ReadInstance();
    if (g_instance[0]) {
        snprintf(p, sizeof(p), "%slockstep_status_%s.txt", g_dataDir, g_instance);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (f) {
            char line[512] = {0};
            if (fgets(line, sizeof(line), f) && strstr(line, " cm=companies")) cached = true;
            fclose(f);
        }
    }
    if (!cached) {
        snprintf(p, sizeof(p), "%smp_company_cfg.txt", g_dataDir);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (f) {
            char line[64] = {0};
            if (fgets(line, sizeof(line), f)) cached = strncmp(line, "companies", 9) == 0;
            fclose(f);
        }
    }
    return cached;
}

// The engine's comparison, with one extra answer. Returns 1 = accept (the line
// editor may add this stop), 0 = reject (what the engine would have said).
// STATION PERMISSIONS (2026-09-16): mp_company_perms.txt, written by the mod
// (companies.lua CM.cmWritePerms) from lockstep state:
//   pid <playerEntity> <companyId>      one per company
//   open <companyId> *|-|<id>,<id>,...  what that company's stations are open to
// Answers: is the owner's company open to ours? Unknown entities or a missing
// file answer yes (the behaviour before the file existed); a stale file is at
// most 2 s old, and every instance re-checks the line update it applies with
// the same lockstep state (lines.lua), so a race here cannot split the worlds.
static bool SharedStationsPermitted(int owner, int mine)
{
    static ULONGLONG last = 0;
    static int pidN = 0;
    static int pids[256], cids[256];
    static char open[256][96];      // per company id 1..255: "*", "-" or a list
    const ULONGLONG now = GetTickCount64();
    if (!last || now - last >= 2000) {
        last = now;
        pidN = 0;
        memset(open, 0, sizeof(open));
        if (g_dataDir[0]) {
            char p[MAX_PATH];
            snprintf(p, sizeof(p), "%smp_company_perms.txt", g_dataDir);
            FILE* f = _fsopen(p, "r", _SH_DENYNO);
            if (f) {
                char line[160];
                while (fgets(line, sizeof(line), f)) {
                    int a = 0, b = 0; char code[96] = {0};
                    if (sscanf(line, "pid %d %d", &a, &b) == 2) {
                        if (pidN < 256) { pids[pidN] = a; cids[pidN] = b; pidN++; }
                    } else if (sscanf(line, "open %d %95s", &a, code) == 2) {
                        if (a >= 1 && a < 256) { strncpy(open[a], code, 95); open[a][95] = 0; }
                    }
                }
                fclose(f);
            }
        }
    }
    int ownerCid = 0, mineCid = 0;
    for (int i = 0; i < pidN; i++) {
        if (pids[i] == owner) ownerCid = cids[i];
        if (pids[i] == mine) mineCid = cids[i];
    }
    if (!ownerCid || !mineCid || ownerCid == mineCid) return true;
    if (ownerCid < 1 || ownerCid >= 256 || !open[ownerCid][0]) return true;
    const char* code = open[ownerCid];
    if (!strcmp(code, "*")) return true;
    if (!strcmp(code, "-")) return false;
    // a comma list of company ids
    const char* s = code;
    while (*s) {
        int v = atoi(s);
        if (v == mineCid) return true;
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
    }
    return false;
}

extern "C" int SharedStationsAllow(int owner, int mine)
{
    if (owner == mine) return 1;
    InterlockedIncrement(&g_ssCalls);
    int live = 0;
    __try { live = SharedStationsCompaniesLive() ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { live = 0; }
    if (!live) return 0;
    int permitted = 1;
    __try { permitted = SharedStationsPermitted(owner, mine) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { permitted = 1; }
    if (!permitted) {
        static bool saidRefuse = false;
        InterlockedIncrement(&g_ssRefused);
        if (!saidRefuse) {
            saidRefuse = true;
            Log("[sharedstations] line editor refused a stop owned by player %d (we are %d): its company's "
                "stations are not open to ours (mp_company_perms.txt)\n", owner, mine);
        }
        return 0;
    }
    InterlockedIncrement(&g_ssOpened);
    if (!g_ssSaidOnce) {
        g_ssSaidOnce = true;
        Log("[sharedstations] line editor accepted a stop owned by player %d (we are %d) -- "
            "companies mode is live; the line, its vehicles and its income stay with us\n",
            owner, mine);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// PAUSED TICK -- a counter that advances per render batch while paused.
//
// ecs::component::GameTime carries two counters, both saved. +0x34 counts sim
// iterations. +0x30 counts them too -- and, in the PAUSED branch of
// GameSim::Step (0x15aa39: speed 0 -> 0xaea970(engine, timeEntity, false)),
// one more per render batch, i.e. at each machine's own frame rate. Every
// pause -- a speed vote of 0, the load gate a hot joiner sits in, a catch-up
// hold, the gap hold -- therefore leaves +0x30 a machine-specific number of
// batches ahead, for good, because the save carries it.
//
// The sim reads +0x30 through the accessor 0x2877c0 in: TownDeveloper::Develop
// (0x943c7d, via 0x9439f0) and town creation (0x9372d6, via 0x937180), which
// stamp {+0x30, -1} into every town building and street they propose;
// street_developer_util 0x987f89 and MakeStreetProposal 0xa1a60f, the same
// stamp; AccountSystem::Update 0xa26af1, `+0x30 % accounts`, which account this
// step processes; TrainMoveSystem::Update2 0xabe035, its shuffle seed (and the
// TRAIN RESERVATION ORDER jitter above reads the same field). Two peers that
// ever paused for a different number of frames -- a hot joiner always has --
// therefore stamp, pick and seed differently from then on, while every world
// hash still matches: the towns then grow apart (the same-save, no-command
// splits of 2026-09-16). Persons and industries seed from +0x34 and were never
// affected.
//
// The fix is one call: the paused branch's increment is NOPed, so +0x30 only
// ever advances with +0x34, in lockstep. A save written before this build
// carries whatever skew it had; every peer that loads it starts from the same
// value, so it is a constant, not a divergence. The call's only other effect
// was the component's change notification while paused, which nothing in the
// sim can observe (the sim is not stepping).
// KILL SWITCH: `pausedtick=0` in tpf2_menu_flags.txt.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_PAUSED_TICK_SITE = 0x15aa39;   // the paused branch, GameSim::Step
static const uintptr_t RVA_PAUSED_TICK_CALL = 0x15aa4a;   // call 0xaea970 (bool = false)
static const uintptr_t RVA_GAMETIME_ADVANCE = 0xaea970;   // GameTime advance(engine, entity, bool stepped)
static const uint8_t PAUSED_TICK_EXPECT[22] = {
    0x49, 0x8B, 0x4E, 0x08,              // mov rcx, [r14+8]               (0x15aa39)
    0x45, 0x33, 0xC0,                    // xor r8d, r8d   <- stepped = false
    0x8B, 0x91, 0x10, 0x02, 0x00, 0x00,  // mov edx, [rcx+0x210]           (the time entity)
    0x48, 0x8B, 0x49, 0x28,              // mov rcx, [rcx+0x28]            (the ecs engine)
    0xE8, 0x21, 0xFF, 0x98, 0x00         // call 0xaea970                  (0x15aa4a)
};
// ...and the advance itself: `inc dword [rax+0x30]` unconditionally, then
// `test bpl,bpl ; je` around `inc dword [rax+0x34]` -- the bool is the step.
static const uintptr_t RVA_GAMETIME_ADVANCE_INC = 0xaeaa23;
static const uint8_t GAMETIME_ADVANCE_EXPECT[16] = {
    0xFF, 0x40, 0x30,                    // inc dword ptr [rax+0x30]
    0x40, 0x84, 0xED,                    // test bpl, bpl
    0x74, 0x08,                          // je +8
    0x48, 0x8B, 0x44, 0x24, 0x38,        // mov rax, [rsp+0x38]
    0xFF, 0x40, 0x34                     // inc dword ptr [rax+0x34]
};

static void InstallPausedTick()
{
    if (FlagsSayOff("pausedtick")) {
        Log("[pausedtick] OFF (pausedtick=0 in tpf2_menu_flags.txt) -- GameTime+0x30 keeps "
            "advancing per render batch while paused\n");
        return;
    }
    if (!BytesAre(RVA_PAUSED_TICK_SITE, PAUSED_TICK_EXPECT, sizeof(PAUSED_TICK_EXPECT), "pausedtick")) return;
    if (!BytesAre(RVA_GAMETIME_ADVANCE_INC, GAMETIME_ADVANCE_EXPECT, sizeof(GAMETIME_ADVANCE_EXPECT), "pausedtick")) return;
    int32_t rel = 0;
    memcpy(&rel, PAUSED_TICK_EXPECT + 18, 4);
    if ((uintptr_t)((int64_t)RVA_PAUSED_TICK_CALL + 5 + rel) != RVA_GAMETIME_ADVANCE) {
        Log("[pausedtick] NOT installed: the call at rva=%llx does not resolve to the GameTime "
            "advance at %llx\n", (unsigned long long)RVA_PAUSED_TICK_CALL,
            (unsigned long long)RVA_GAMETIME_ADVANCE);
        return;
    }
    const uintptr_t at = g_base + RVA_PAUSED_TICK_CALL;
    DWORD old = 0;
    if (!VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[pausedtick] NOT installed: could not unprotect rva=%llx\n", (unsigned long long)RVA_PAUSED_TICK_CALL);
        return;
    }
    static const uint8_t nop5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };   // one 5-byte nop
    memcpy((void*)at, nop5, 5);
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    Log("[pausedtick] installed: the paused branch of GameSim::Step (rva=%llx) no longer "
        "advances GameTime+0x30 per render batch; it moves only with the step count\n",
        (unsigned long long)RVA_PAUSED_TICK_CALL);
}

// ---------------------------------------------------------------------------
// SHOW ALL ICONS (2026-09-16) -- the world icons over stations and vehicles are
// drawn only for the local player's entities. Three owner tests gate them, one
// per icon system, each a plain cmp/jne (RE pass on build 35924):
//   * ItemCreator::Visit 0x808478 -- vehicles (road/rail/water/air), station
//     name labels, signals, dead ends: cmp eax,[rbx+0x20] ; jne 0x8088de
//   * ItemCreator::End 0x80c569 -- the second gate, trains only:
//     mov eax,[r15+0x20] ; cmp [rdx],eax ; jne 0x80c640
//   * HudIconManager::DoStep lambda 0x5de526 -- the clickable station/depot
//     buttons: cmp [rdx],r15d ; jne 0x5de603
// Opening a gate = NOP its jne, so the accept path runs for every owner. In
// co-op there is one player entity, so the compare always succeeded and the NOP
// changes nothing; only in companies mode do foreign entities now get icons.
// Widening these does not open the entity WINDOWS (UI::ViewCreator 0x8b3020
// still refuses a foreign entity, so a click does nothing) nor the list windows
// (GetEntitiesForPlayer, untouched). The company-colour TINT of a foreign icon
// is a separate render-path detour (0x80b613), done after this is proven live.
// KILL SWITCH: `showicons=0` in tpf2_menu_flags.txt.
// ---------------------------------------------------------------------------
struct IconGate { uintptr_t jne; uint8_t before[8]; int beforeLen; uint8_t jbytes[6]; const char* what; };
static const IconGate ICON_GATES[3] = {
    // the two bytes before each jne are the cmp it depends on: a byte match that
    // landed elsewhere cannot pass. Visit: cmp eax,[rbx+0x20]. End: cmp [rdx],eax.
    // DoStep: cmp [rdx],r15d.
    { 0x80847b, { 0x3B, 0x43, 0x20 }, 3, { 0x0F, 0x85, 0x5D, 0x04, 0x00, 0x00 }, "vehicles, station labels, signals (Visit)" },
    { 0x80c56b, { 0x39, 0x02 },       2, { 0x0F, 0x85, 0xCF, 0x00, 0x00, 0x00 }, "trains (End)" },
    { 0x5de529, { 0x44, 0x39, 0x3A }, 3, { 0x0F, 0x85, 0xD4, 0x00, 0x00, 0x00 }, "station/depot buttons (DoStep)" },
};

static void InstallShowAllIcons()
{
    if (FlagsSayOff("showicons")) {
        Log("[showicons] OFF (showicons=0 in tpf2_menu_flags.txt) -- icons only over your own "
            "stations and vehicles\n");
        return;
    }
    // Verify every gate before touching any: a partial patch (one system opened,
    // two not) is worse than none.
    for (int i = 0; i < 3; i++) {
        const IconGate& g = ICON_GATES[i];
        if (!BytesAre(g.jne - g.beforeLen, g.before, g.beforeLen, "showicons")) return;
        if (!BytesAre(g.jne, g.jbytes, sizeof(g.jbytes), "showicons")) return;
    }
    static const uint8_t NOP6[6] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
    int done = 0;
    for (int i = 0; i < 3; i++) {
        const uintptr_t at = g_base + ICON_GATES[i].jne;
        DWORD old = 0;
        if (!VirtualProtect((void*)at, 6, PAGE_EXECUTE_READWRITE, &old)) {
            Log("[showicons] NOT installed: could not unprotect rva=%llx (%s)\n",
                (unsigned long long)ICON_GATES[i].jne, ICON_GATES[i].what);
            continue;
        }
        memcpy((void*)at, NOP6, 6);
        VirtualProtect((void*)at, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)at, 6);
        done++;
    }
    Log("[showicons] installed: %d/3 owner gates opened -- every player's stations and vehicles "
        "get icons\n", done);
}

// ---------------------------------------------------------------------------
// ICON COLOUR (2026-09-16) -- a foreign vehicle icon is tinted its owner's
// company colour, so you can tell whose it is. The icon quad is drawn by
// AddVehicle (0x80b410) with `call 0x8088f0` at 0x80b613, r9 = colour pointer,
// currently NULL (untinted). A non-null r9 -> CVec4f (4 floats, copied into the
// vertex buffer during the call) modulates the texture. The call site is a
// clean 5-byte `e8 rel32`; we keep it a CALL (so 0x8088f0 returns to 0x80b618)
// and point its rel32 at a stub that fills r9 for a foreign owner. The owner is
// read fault-safely via the game's own GetComponentPtr<PlayerOwned> (0x472900,
// returns NULL on a missing/edge entity rather than faulting). pid -> company
// comes from mp_company_perms.txt (the mod writes it, companies.lua); the colour
// per company is the lobby chip colour. Tints vehicle icons of all four carrier
// types and nothing else (0x8088f0's only AddVehicle caller is this site).
// KILL SWITCH: `iconcolor=0` in tpf2_menu_flags.txt.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_ICON_DRAW_CALL   = 0x80b613;   // call 0x8088f0 in AddVehicle
static const uintptr_t RVA_ICON_DRAW_TARGET = 0x8088f0;   // AddQuad(rect, buffer, tex, colour*, ...)
static const uintptr_t RVA_GET_PLAYEROWNED  = 0x472900;   // GetComponentPtr<PlayerOwned>(engine, &entity)
static const uint8_t ICON_DRAW_EXPECT[5] = { 0xE8, 0xD8, 0xD2, 0xFF, 0xFF };
static bool g_iconColorOn = false;

// pid -> company id, from mp_company_perms.txt ("pid <playerEntity> <companyId>"),
// cached 2 s. 0 = unknown (coop, or a pid with no company). Its own cache, so it
// never disturbs SharedStationsPermitted's.
static int IconCompanyOfPid(int pid)
{
    static ULONGLONG last = 0;
    static int n = 0;
    static int pids[256], cids[256];
    const ULONGLONG now = GetTickCount64();
    if (!last || now - last >= 2000) {
        last = now;
        n = 0;
        if (g_dataDir[0]) {
            char p[MAX_PATH];
            snprintf(p, sizeof(p), "%smp_company_perms.txt", g_dataDir);
            FILE* f = _fsopen(p, "r", _SH_DENYNO);
            if (f) {
                char line[160];
                while (fgets(line, sizeof(line), f)) {
                    int a = 0, b = 0;
                    if (sscanf(line, "pid %d %d", &a, &b) == 2 && n < 256) { pids[n] = a; cids[n] = b; n++; }
                }
                fclose(f);
            }
        }
    }
    for (int i = 0; i < n; i++) if (pids[i] == pid) return cids[i];
    return 0;
}

// The company's colour, 0..1 RGB -- the 20 distinct lobby-chip colours (Trubetskoy) and the
// golden-angle hue walk, byte-for-byte the menu's coColor / companies.lua
// CM.cmCompanyColor, so an icon matches its roster chip.
static void IconCompanyColor(int cid, float out[3])
{
    static const int first[20][3] = { {230,25,75}, {0,130,200}, {60,180,75}, {245,130,48}, {145,30,180}, {70,240,240}, {240,50,230}, {255,225,25}, {0,128,128}, {170,110,40}, {210,245,60}, {128,0,0}, {0,0,128}, {128,128,0}, {250,190,212}, {220,190,255}, {170,255,195}, {255,215,180}, {128,128,128}, {255,250,200} };
    if (cid >= 1 && cid <= 20) {
        out[0] = first[cid - 1][0] / 255.0f; out[1] = first[cid - 1][1] / 255.0f; out[2] = first[cid - 1][2] / 255.0f;
        return;
    }
    double hd = ((cid - 21) * 137.508);
    float h = (float)(hd - (int)(hd / 360.0) * 360.0);
    if (h < 0) h += 360.0f;
    const float sat = 0.62f, val = 0.85f, c = val * sat;
    const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = val - c;
    float r, g, b;
    if (h < 60)      { r = c; g = x; b = 0; }
    else if (h < 120){ r = x; g = c; b = 0; }
    else if (h < 180){ r = 0; g = c; b = x; }
    else if (h < 240){ r = 0; g = x; b = c; }
    else if (h < 300){ r = x; g = 0; b = c; }
    else             { r = c; g = 0; b = x; }
    out[0] = r + m; out[1] = g + m; out[2] = b + m;
}

// Called by the stub for every vehicle icon. Returns a pointer to 4 floats
// (RGBA, alpha 1) to tint a FOREIGN owner's icon, or NULL to leave it untinted
// (no owner, own vehicle, or coop). A fault reading the component is swallowed:
// an untinted icon is never worth a render-thread crash. The buffer is a single
// static -- one render thread, and 0x8088f0 copies it before it returns.
// The ecs engine and the local player, cached from the icon path (which has them
// reliably every frame at ItemCreatorImpl+0x28/+0x20). The window tint (0x8b2390)
// and the station-label tint have no engine in hand at their sites; they read
// these. A per-world pointer that only changes on a new game / load, and windows
// and labels only render while the icon path is running, so it is fresh; a stale
// value just yields no tint (the lookups are SEH-guarded).
static volatile void*  g_uiEngine = nullptr;
static volatile LONG   g_uiLocalPlayer = -1;
static void IconEngineSeen(void* engine);   // STATION ICON COLOUR below: the engine of the item being built, with its time
static uint8_t* IconEngineRecent(unsigned maxAgeMs);   // ...and that engine, if seen within maxAgeMs (never a dead world's)

extern "C" const float* IconTintForEntity(void* engine, const int* entity, int local)
{
    static float rgba[4];
    if (engine) { g_uiEngine = engine; IconEngineSeen(engine); InterlockedExchange(&g_uiLocalPlayer, local); }
    __try {
        typedef void* (*GetPlayerOwned)(void*, const int*);
        void* po = ((GetPlayerOwned)(g_base + RVA_GET_PLAYEROWNED))(engine, entity);
        if (!po) return nullptr;
        const int owner = *(const int*)po;
        // ICONS SHOW EVERY COMPANY'S COLOUR, OWN INCLUDED (2026-09-16): the icon
        // and station-label tints colour your OWN vehicles/stations your company's
        // colour too, not just other companies'. Only unowned entities (owner < 0,
        // towns/industries) and coop (no company for the pid -> cid 0 below) stay
        // untinted. The read-only WINDOW wash stays foreign-only (WindowTint keeps
        // its owner == local skip). `local` is still cached above for WindowTint.
        (void)local;
        if (owner < 0) return nullptr;
        const int cid = IconCompanyOfPid(owner);
        if (cid <= 0) return nullptr;
        IconCompanyColor(cid, rgba);
        rgba[3] = 1.0f;
        return rgba;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void InstallIconColor()
{
    if (FlagsSayOff("iconcolor")) {
        Log("[iconcolor] OFF (iconcolor=0 in tpf2_menu_flags.txt) -- foreign vehicle icons are "
            "not tinted\n");
        return;
    }
    if (!BytesAre(RVA_ICON_DRAW_CALL, ICON_DRAW_EXPECT, sizeof(ICON_DRAW_EXPECT), "iconcolor")) return;
    int32_t rel = 0;
    memcpy(&rel, ICON_DRAW_EXPECT + 1, 4);
    if ((uintptr_t)((int64_t)RVA_ICON_DRAW_CALL + 5 + rel) != RVA_ICON_DRAW_TARGET) {
        Log("[iconcolor] NOT installed: the call at rva=%llx does not resolve to the icon quad "
            "draw %llx\n", (unsigned long long)RVA_ICON_DRAW_CALL, (unsigned long long)RVA_ICON_DRAW_TARGET);
        return;
    }
    // The stub: fill r9 with the owner's company colour, then jmp the real draw.
    // Entered by CALL (rel32 rewritten below), so [rsp] = 0x80b618 and the draw's
    // ret lands back in AddVehicle. rcx/rdx/r8 are the draw's live args -> saved.
    uint8_t* stub = NearAlloc(96);
    if (!stub) { Log("[iconcolor] NOT installed: no page within reach for the stub\n"); return; }
    const uintptr_t helper = (uintptr_t)&IconTintForEntity;
    const uintptr_t target = g_base + RVA_ICON_DRAW_TARGET;
    size_t k = 0;
    stub[k++] = 0x51;                                                        // push rcx
    stub[k++] = 0x52;                                                        // push rdx
    stub[k++] = 0x41; stub[k++] = 0x50;                                      // push r8
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xEC; stub[k++] = 0x20;  // sub rsp, 0x20
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0x4E; stub[k++] = 0x28;  // mov rcx, [rsi+0x28]  (engine)
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xD5;                    // mov rdx, rbp         (&entity)
    stub[k++] = 0x44; stub[k++] = 0x8B; stub[k++] = 0x46; stub[k++] = 0x20;  // mov r8d, [rsi+0x20]  (local player)
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &helper, 8); k += 8;// mov rax, IconTintForEntity
    stub[k++] = 0xFF; stub[k++] = 0xD0;                                      // call rax
    stub[k++] = 0x49; stub[k++] = 0x89; stub[k++] = 0xC1;                    // mov r9, rax
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xC4; stub[k++] = 0x20;  // add rsp, 0x20
    stub[k++] = 0x41; stub[k++] = 0x58;                                      // pop r8
    stub[k++] = 0x5A;                                                        // pop rdx
    stub[k++] = 0x59;                                                        // pop rcx
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &target, 8); k += 8;// mov rax, 0x8088f0
    stub[k++] = 0xFF; stub[k++] = 0xE0;                                      // jmp rax
    FlushInstructionCache(GetCurrentProcess(), stub, k);
    // Rewrite the call's rel32 to the stub; keep the 0xE8 (still a CALL).
    const uintptr_t at = g_base + RVA_ICON_DRAW_CALL;
    const int64_t nrel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) {
        Log("[iconcolor] NOT installed: the stub is out of rel32 reach of the call site\n");
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect((void*)(at + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[iconcolor] NOT installed: could not unprotect the call at rva=%llx\n",
            (unsigned long long)RVA_ICON_DRAW_CALL);
        return;
    }
    const int32_t r32 = (int32_t)nrel;
    memcpy((void*)(at + 1), &r32, 4);
    VirtualProtect((void*)(at + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    g_iconColorOn = true;
    Log("[iconcolor] installed: foreign vehicle icons tinted their owner's company colour "
        "(call at rva=%llx -> stub)\n", (unsigned long long)RVA_ICON_DRAW_CALL);
}

// ---------------------------------------------------------------------------
// STATION LABEL COLOUR (2026-09-16) -- a foreign station's world name-label
// background is washed the owner's company colour, the station counterpart of
// the vehicle-icon tint. The label background is drawn at 0x80a0ee (call 0x8090f0,
// r8 = colour pointer, chosen by cmove between grey [rbp+0x98] and the blue
// highlight [rbp+0xa8]). At that call r13 = ItemCreatorImpl (engine [r13+0x28],
// local [r13+0x20]) and r12 = &entity (the station id, the same pointer handed to
// the click-rect register at 0x80a0fc). For a foreign owner the stub overrides r8
// with the company colour (IconTintForEntity, the vehicle-icon helper); own/coop
// keep the grey/blue. Same fault-safe lookup, same `iconcolor` kill switch.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_STNLABEL_CALL   = 0x80a0ee;   // call 0x8090f0 (label bg) in the label builder
static const uintptr_t RVA_STNLABEL_TARGET = 0x8090f0;   // AddRect(buffer, tex, colour*, rect)
static const uint8_t STNLABEL_EXPECT[5] = { 0xE8, 0xFD, 0xEF, 0xFF, 0xFF };
static bool g_stnLabelColorOn = false;
static volatile LONG g_slAsked = 0, g_slTinted = 0, g_slShown = 0;

// The label site's own counted wrapper around the vehicle-icon helper, so the log
// can say whether the station-label draw is reached at all and what it decides.
extern "C" const float* StationLabelTint(void* engine, const int* entity, int local)
{
    InterlockedIncrement(&g_slAsked);
    const float* c = IconTintForEntity(engine, entity, local);
    if (c) InterlockedIncrement(&g_slTinted);
    if (InterlockedIncrement(&g_slShown) <= 6) {
        int ent = -1; __try { if (entity) ent = *entity; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("[stationlabelcolor] label for entity %d (local %d): %s\n", ent, local,
            c ? "company colour" : "no colour (no owner / coop / own-and-off)");
    }
    return c;
}

static void InstallStationLabelColor()
{
    if (FlagsSayOff("iconcolor")) return;   // same switch as the vehicle-icon tint
    if (!BytesAre(RVA_STNLABEL_CALL, STNLABEL_EXPECT, sizeof(STNLABEL_EXPECT), "stationlabelcolor")) return;
    int32_t rel = 0;
    memcpy(&rel, STNLABEL_EXPECT + 1, 4);
    if ((uintptr_t)((int64_t)RVA_STNLABEL_CALL + 5 + rel) != RVA_STNLABEL_TARGET) {
        Log("[stationlabelcolor] NOT installed: the call at rva=%llx does not resolve to the label "
            "draw %llx\n", (unsigned long long)RVA_STNLABEL_CALL, (unsigned long long)RVA_STNLABEL_TARGET);
        return;
    }
    uint8_t* stub = NearAlloc(96);
    if (!stub) { Log("[stationlabelcolor] NOT installed: no page for the stub\n"); return; }
    const uintptr_t helper = (uintptr_t)&StationLabelTint;
    const uintptr_t target = g_base + RVA_STNLABEL_TARGET;
    size_t k = 0;
    stub[k++] = 0x51;                                                        // push rcx (buffer)
    stub[k++] = 0x52;                                                        // push rdx (tex)
    stub[k++] = 0x41; stub[k++] = 0x50;                                      // push r8  (default colour)
    stub[k++] = 0x41; stub[k++] = 0x51;                                      // push r9  (rect)
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xEC; stub[k++] = 0x28;  // sub rsp, 0x28
    stub[k++] = 0x49; stub[k++] = 0x8B; stub[k++] = 0x4D; stub[k++] = 0x28;  // mov rcx, [r13+0x28]  (engine)
    stub[k++] = 0x4C; stub[k++] = 0x89; stub[k++] = 0xE2;                    // mov rdx, r12         (&entity)
    stub[k++] = 0x45; stub[k++] = 0x8B; stub[k++] = 0x45; stub[k++] = 0x20;  // mov r8d, [r13+0x20]  (local)
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &helper, 8); k += 8;// mov rax, IconTintForEntity
    stub[k++] = 0xFF; stub[k++] = 0xD0;                                      // call rax
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xC4; stub[k++] = 0x28;  // add rsp, 0x28
    stub[k++] = 0x41; stub[k++] = 0x59;                                      // pop r9
    stub[k++] = 0x41; stub[k++] = 0x58;                                      // pop r8
    stub[k++] = 0x5A;                                                        // pop rdx
    stub[k++] = 0x59;                                                        // pop rcx
    stub[k++] = 0x48; stub[k++] = 0x85; stub[k++] = 0xC0;                    // test rax, rax
    stub[k++] = 0x74; stub[k++] = 0x03;                                      // je +3 (keep default r8)
    stub[k++] = 0x49; stub[k++] = 0x89; stub[k++] = 0xC0;                    // mov r8, rax  (company colour)
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &target, 8); k += 8;// mov rax, 0x8090f0
    stub[k++] = 0xFF; stub[k++] = 0xE0;                                      // jmp rax
    FlushInstructionCache(GetCurrentProcess(), stub, k);
    const uintptr_t at = g_base + RVA_STNLABEL_CALL;
    const int64_t nrel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) { Log("[stationlabelcolor] NOT installed: stub out of reach\n"); return; }
    DWORD old = 0;
    if (!VirtualProtect((void*)(at + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[stationlabelcolor] NOT installed: could not unprotect rva=%llx\n", (unsigned long long)RVA_STNLABEL_CALL);
        return;
    }
    const int32_t r32 = (int32_t)nrel;
    memcpy((void*)(at + 1), &r32, 4);
    VirtualProtect((void*)(at + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    g_stnLabelColorOn = true;
    Log("[stationlabelcolor] installed: a foreign station's name-label background is washed the "
        "owner's company colour (call at rva=%llx)\n", (unsigned long long)RVA_STNLABEL_CALL);
}

// ---------------------------------------------------------------------------
// FOREIGN WINDOWS (2026-09-16) -- clicking a foreign station/vehicle/depot icon
// opens its info window (read-only). UI::ViewCreator::CanCreateView (0x8b3020)
// is a PURE predicate: it reads GetComponentPtr<PlayerOwned> and, for a foreign
// owner, returns 0 (no window) before the type cascade -- the gate at 0x8b3060.
// Opening it (NOP the jne) lets a foreign entity's window build; building only
// READS components, so no write, no command, no sim/lockstep effect, and the
// clicked entity is null-checked by 0x472900. The depot and construction windows
// already suppress their edit blocks for a foreign owner; the vehicle and
// station-group windows do NOT, so their edit controls are made inert on the
// originator by the mod's capture guard (inject.lua CM.injForeignEdit). Together
// that is a genuinely read-only foreign window that cannot desync.
// KILL SWITCH: `foreignwindows=0` in tpf2_menu_flags.txt.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_FOREIGNWIN_JNE = 0x8b3060;   // cmp [rax],edx ; jne 0x8b3388 (reject: return 0)
// the two bytes before are the owner compare it depends on: cmp dword [rax],edx
static const uint8_t FOREIGNWIN_BEFORE[2] = { 0x39, 0x10 };
static const uint8_t FOREIGNWIN_JNE_BYTES[6] = { 0x0F, 0x85, 0x22, 0x03, 0x00, 0x00 };

static void InstallForeignWindows()
{
    if (FlagsSayOff("foreignwindows")) {
        Log("[foreignwindows] OFF (foreignwindows=0 in tpf2_menu_flags.txt) -- a foreign entity's "
            "window cannot be opened\n");
        return;
    }
    if (!BytesAre(RVA_FOREIGNWIN_JNE - 2, FOREIGNWIN_BEFORE, sizeof(FOREIGNWIN_BEFORE), "foreignwindows")) return;
    if (!BytesAre(RVA_FOREIGNWIN_JNE, FOREIGNWIN_JNE_BYTES, sizeof(FOREIGNWIN_JNE_BYTES), "foreignwindows")) return;
    static const uint8_t NOP6[6] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
    const uintptr_t at = g_base + RVA_FOREIGNWIN_JNE;
    DWORD old = 0;
    if (!VirtualProtect((void*)at, 6, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[foreignwindows] NOT installed: could not unprotect rva=%llx\n",
            (unsigned long long)RVA_FOREIGNWIN_JNE);
        return;
    }
    memcpy((void*)at, NOP6, 6);
    VirtualProtect((void*)at, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 6);
    Log("[foreignwindows] installed: a foreign entity's info window opens read-only (the mod's "
        "capture guard keeps its edit controls inert)\n");
}

// ---------------------------------------------------------------------------
// WINDOW COLOUR (2026-09-16) -- a foreign entity's (read-only) info window is
// washed with its owner's company colour, so it's obvious whose it is. All 14
// entity-view creators funnel through the bind helper 0x8b2390(window, entityId)
// once when a window opens. Windows are 100% style-sheet driven (no native RGBA
// write); a window tags itself with classes via addStyleClass 0x227a1e0(window,
// std::string*), which appends only if absent. So for a foreign entity we append
// "!mpWinCoN" (a translucent company wash defined in res/config/style_sheet/
// mp_lockstep.lua). Once per window open, rendering-only, cannot desync; the
// owner is read via 0x472900 (null-safe) with the engine/local cached from the
// icon path (g_uiEngine/g_uiLocalPlayer). KILL SWITCH: `windowcolor=0`.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_ADD_STYLE_CLASS = 0x227a1e0;   // CComponent::addStyleClass(this, std::string*)
static const uintptr_t RVA_WINDOW_BIND     = 0x8b2390;     // bind entity to window (all view creators)
static const uint8_t WINDOW_BIND_EXPECT[9] = {
    0x40, 0x53,                                 // push rbx
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00    // sub rsp, 0x80
};
static bool g_windowColorOn = false;

// Which class the washes append: "mpWinCo" (translucent background, the
// default) or "mpCo" (the opaque chip class) -- `tintclass=mpCo` in
// tpf2_menu_flags.txt picks the opaque one, so the next run can try the other
// without a rebuild if a translucent root background turns out not to paint.
static const char* TintClassPrefix()
{
    static int which = -1;
    if (which < 0) {
        which = 0;
        for (int i = 0; i < 2; i++) {
            const char* dir = i == 0 ? g_dllDir : g_dataDir;
            if (!dir[0]) continue;
            char p[MAX_PATH];
            snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", dir);
            FILE* f = _fsopen(p, "r", _SH_DENYNO);
            if (!f) continue;
            char line[256];
            while (fgets(line, sizeof(line), f)) if (!strncmp(line, "tintclass=mpCo", 14)) which = 1;
            fclose(f);
            break;
        }
    }
    // NO BANG (2026-09-16, the reason three builds painted nothing): in the sheet
    // "StationItem::StationIcon!train" the '!' is selector syntax; the class the
    // game stores on the element is "train" (read back: "train !mpWinCo4" -- ours
    // never matched a rule). The dashboard swatches set "mpCo3" the same way.
    return which == 1 ? "mpCo" : "mpWinCo";
}

// The component's style-class list, as the game keeps it: std::string records
// (MSVC, 0x20 bytes) between [comp+0xb0] and [comp+0xb8]. Read back after an
// append so the log says whether the class really landed (the mechanism was
// inferred from bytes; this is the check). Writes "a b c" into out.
static void TintClassList(const void* comp, char* out, size_t cap)
{
    out[0] = 0;
    const uint8_t* c = (const uint8_t*)comp;
    if (!Readable(c + 0xb0, 16)) { snprintf(out, cap, "(unreadable)"); return; }
    const uint8_t* b = *(const uint8_t* const*)(c + 0xb0);
    const uint8_t* e = *(const uint8_t* const*)(c + 0xb8);
    if (!b || e < b || (size_t)(e - b) % 0x20 || (size_t)(e - b) > 0x20 * 64) { snprintf(out, cap, "(odd list %p..%p)", b, e); return; }
    size_t n = 0;
    for (const uint8_t* r = b; r < e && n + 2 < cap; r += 0x20) {
        if (!Readable(r, 0x20)) break;
        uint64_t sz = 0, cp = 0; memcpy(&sz, r + 0x10, 8); memcpy(&cp, r + 0x18, 8);
        const char* s = (const char*)r;
        if (cp >= 16) { uint64_t ptr = 0; memcpy(&ptr, r, 8); s = (const char*)ptr; }
        if (sz > 64 || !s || !Readable(s, (size_t)sz)) break;
        if (n) out[n++] = ' ';
        size_t take = (size_t)sz; if (n + take + 1 >= cap) take = cap - n - 1;
        memcpy(out + n, s, take); n += take; out[n] = 0;
    }
}

static volatile LONG g_wcAsked = 0, g_wcTinted = 0, g_wcFaults = 0;
static volatile LONG g_siAsked = 0, g_siDirect = 0, g_siWalked = 0, g_siTinted = 0, g_siFaults = 0, g_siNoOwner = 0;

// no SEH in this scope (it constructs a std::string with a destructor): the
// callers wrap the call in __try. Logs the first few per tag with the class
// list read back, so a run says whether the class landed on the component.
static void TintApplyClass(void* comp, int cid, const char* tag, int entity, int owner, volatile LONG* shown)
{
    std::string cls = std::string(TintClassPrefix()) + std::to_string(cid);
    typedef void (*AddClass)(void*, const void*);
    ((AddClass)(g_base + RVA_ADD_STYLE_CLASS))(comp, &cls);
    if (InterlockedIncrement(shown) <= 4) {
        char list[512];
        TintClassList(comp, list, sizeof(list));
        Log("[%s] entity %d owner %d -> company %d: appended %s; the component's classes now: %s\n",
            tag, entity, owner, cid, cls.c_str(), list);
    }
}
static volatile LONG g_wcShown = 0, g_siShown = 0, g_siNoOwnerShown = 0, g_wcSeen = 0;

extern "C" void WindowTint(void* window, int entity)
{
    InterlockedIncrement(&g_wcAsked);
    if (InterlockedIncrement(&g_wcSeen) <= 6) Log("[windowcolor] window bind for entity %d\n", entity);
    __try {
        void* engine = IconEngineRecent(5000);
        if (!engine || !window) return;
        const int local = (int)InterlockedCompareExchange(&g_uiLocalPlayer, 0, 0);
        int ent = entity;
        typedef void* (*GetPlayerOwned)(void*, const int*);
        void* po = ((GetPlayerOwned)(g_base + RVA_GET_PLAYEROWNED))(engine, &ent);
        if (!po) return;
        const int owner = *(const int*)po;
        if (owner < 0 || owner == local) return;   // unowned or ours: no wash
        const int cid = IconCompanyOfPid(owner);
        if (cid <= 0) return;
        TintApplyClass(window, cid, "windowcolor", entity, owner, &g_wcShown);
        InterlockedIncrement(&g_wcTinted);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_wcFaults); }
}

static void InstallWindowColor()
{
    if (FlagsSayOff("windowcolor")) {
        Log("[windowcolor] OFF (windowcolor=0 in tpf2_menu_flags.txt) -- foreign windows are not "
            "washed with the owner's colour\n");
        return;
    }
    if (!BytesAre(RVA_WINDOW_BIND, WINDOW_BIND_EXPECT, sizeof(WINDOW_BIND_EXPECT), "windowcolor")) return;
    // A stub that tints (rcx=window, edx=entity), then the trampoline runs the two
    // stolen instructions and jumps to bind+9. Keep rcx/rdx across the call: the
    // window body after bind+9 reads rcx (mov rbx,rcx) and edx (the entity).
    void* tramp = nullptr;
    uint8_t* stub = NearAlloc(64);
    if (!stub) { Log("[windowcolor] NOT installed: no page within reach for the stub\n"); return; }
    const uintptr_t helper = (uintptr_t)&WindowTint;
    size_t k = 0;
    stub[k++] = 0x51;                                                        // push rcx  (window)
    stub[k++] = 0x52;                                                        // push rdx  (entity in edx)
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xEC; stub[k++] = 0x28;  // sub rsp, 0x28
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &helper, 8); k += 8;// mov rax, WindowTint
    stub[k++] = 0xFF; stub[k++] = 0xD0;                                      // call rax  (rcx,edx already set)
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xC4; stub[k++] = 0x28;  // add rsp, 0x28
    stub[k++] = 0x5A;                                                        // pop rdx
    stub[k++] = 0x59;                                                        // pop rcx
    // jmp trampoline (filled after PatchJumpNear gives its address)
    const size_t jmpAt = k;
    stub[k++] = 0x48; stub[k++] = 0xB8; memset(stub + k, 0, 8); k += 8;      // mov rax, <tramp>
    stub[k++] = 0xFF; stub[k++] = 0xE0;                                      // jmp rax
    if (!PatchJumpNear(g_base + RVA_WINDOW_BIND, stub, sizeof(WINDOW_BIND_EXPECT), &tramp) || !tramp) {
        Log("[windowcolor] NOT installed: could not write the detour at rva=%llx\n",
            (unsigned long long)RVA_WINDOW_BIND);
        return;
    }
    const uintptr_t tp = (uintptr_t)tramp;
    DWORD old = 0;
    VirtualProtect(stub, 64, PAGE_EXECUTE_READWRITE, &old);
    memcpy(stub + jmpAt + 2, &tp, 8);
    VirtualProtect(stub, 64, old, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    g_windowColorOn = true;
    Log("[windowcolor] installed: a foreign entity's window is washed with the owner's company "
        "colour (bind at rva=%llx)\n", (unsigned long long)RVA_WINDOW_BIND);
}

// ---------------------------------------------------------------------------
// STATION ICON COLOUR (2026-09-16) -- the clickable HUD station/depot icon is
// washed its owner's company colour. HudIconManager::DoStep builds the button
// content with FUN_5e45d0(context, entityId) and, right after that call at
// 0x5e38d0, rax = the item content component and ebx = the entity id (main
// thread, once per icon). The glyph is style-driven (no native RGBA); a company
// class appended via addStyleClass 0x227a1e0 tints it, the same as the window
// wash. Owner resolution: 0x472900 works on depots directly; a StationGroup's
// icon entity has no PlayerOwned, so walk group -> stations[0] -> PlayerOwned
// (StationGroup is a vector<Entity> at component +0; type index via 0xd0a40 on
// engine+0x48 with the StationGroup type_info; component via GetComponentPtr
// 0x149290). Own included (icons show every company; only unowned entities and
// coop stay untinted). KILL SWITCH: `stationicon=0`.
// ---------------------------------------------------------------------------
static const uintptr_t RVA_ICON_STN_HOOK      = 0x5e38e1;   // right after the ItemButton wrap (call 0x2251620) in DoStep
static const uintptr_t RVA_TI_STATIONGROUP    = 0x41d1438;  // .?AUStationGroup@component@ecs@@ descriptor
static const uintptr_t RVA_GET_TYPEINDEX      = 0x0d0a40;   // int(componentMgr = engine+0x48, type_info**)
static const size_t    STATIONGROUP_STRIDE    = 0x18;       // the component is one vector<Entity>: begin/end/cap
static const uint8_t ICON_STN_EXPECT[6] = { 0x48, 0x8B, 0xF8, 0x45, 0x33, 0xE4 };  // mov rdi,rax ; xor r12d,r12d
static bool g_stnIconColorOn = false;

// A component of any stride, the way TrainOrderComponent reads a Name (stride
// 0x20): the flat array at pool+0x68, or the paged table past 0x40000000.
static const uint8_t* EcsComponentAt(uint8_t* world, int typeIdx, int slot, size_t stride)
{
    if (typeIdx < 0 || typeIdx > 4096 || slot < 0) return nullptr;
    uint8_t* pools = *(uint8_t**)(world + 0x88);
    if (!pools) return nullptr;
    uint8_t* pool = *(uint8_t**)(pools + (size_t)typeIdx * 8);
    if (!pool) return nullptr;
    if (slot < 0x40000000) {
        uint8_t* data = *(uint8_t**)(pool + 0x68);
        return data ? data + (size_t)slot * stride : nullptr;
    }
    const int32_t e = slot - 0x40000000;
    uint8_t* pages = *(uint8_t**)(pool + 0x80);
    if (!pages) return nullptr;
    uint8_t* page = *(uint8_t**)(pages + (size_t)(e / 32) * 2 * 8);
    return page ? page + (size_t)(e % 32) * stride : nullptr;
}

// The owner of a HUD icon's entity: its PlayerOwned or, for a station group,
// its first station's. -1 when there is none (a town, an industry). engine is
// g_uiEngine (cached from the icon path).
//
// NEVER the engine's GetComponentPtr for the group (0x149290 -> 0xd0920): that
// one ASSERTS when the entity lacks the component, and this path sees every HUD
// icon entity -- towns and industries have no PlayerOwned and no StationGroup,
// so each of them wrote a crash dump (Engine.h:291 `it != components.end()`),
// which is the 24 s freeze the first build of this caused (2026-09-16). The
// slot scan (TrainOrderSlot) returns -1 on a miss instead.
// THE ENGINE AT LOAD (2026-09-16, sixth build). g_uiEngine is cached by the
// vehicle-icon draw hook, so a HUD built before any vehicle icon drew (a fresh
// load: asked=18 direct=0 glyphs=0) found no engine and tagged nothing -- the
// icons stayed vanilla until something rebuilt them. The StationItem constructor
// receives the UI::EnginePtr as its 2nd argument; the entry pre-hook records it
// and the game's own accessor 0x8b9e60(&ptr) ((*ptr)->vslot1()->+0x28, what DoStep
// itself uses before GetComponentDataIndex) yields the ecs engine from it.
static void* volatile g_curIconEnginePtr = nullptr;          // set at the StationItem / VehicleDepotItem ctor entry (rdx / rcx)
static const uintptr_t RVA_ENGINE_FROM_PTR = 0x8b9e60;       // engine* EngineFromPtr(const EnginePtr*)
// NEVER A CACHE ACROSS WORLDS (2026-09-17). A frozen join reloads the world in
// place; the engine of the world before it is freed. g_uiEngine, cached by the
// vehicle-icon draw, outlived it: the first HUD build of the new world walked
// the dead engine (asked=60 direct=0 noOwner=0 in the crashed run) and on the
// third reload both games crashed at the same second, in the load. So the
// engine is derived from the constructor's OWN EnginePtr, inside that
// constructor while the pointer is live (IconEngineNow), and remembered only
// as "the engine of the item being built", with the time it was seen.
static void* volatile g_curIconEngine = nullptr;
static volatile LONGLONG g_curIconEngineAt = 0;
static void IconEngineSeen(void* engine)
{
    if (!engine) return;
    g_curIconEngine = engine;
    g_curIconEngineAt = (LONGLONG)GetTickCount64();
}
static uint8_t* IconEngineNow()   // inside a constructor: its EnginePtr is live
{
    void* ep = g_curIconEnginePtr;
    if (!ep) return nullptr;
    typedef void* (*EngineFromPtr)(void*);
    void* engine = ((EngineFromPtr)(g_base + RVA_ENGINE_FROM_PTR))(&ep);
    IconEngineSeen(engine);
    return (uint8_t*)engine;
}
static uint8_t* IconEngineRecent(unsigned maxAgeMs)   // the last engine seen, if seen recently enough
{
    void* e = g_curIconEngine;
    if (!e || (LONGLONG)GetTickCount64() - g_curIconEngineAt > (LONGLONG)maxAgeMs) return nullptr;
    return (uint8_t*)e;
}
static int IconOwnerForEntity(uint8_t* engine, int entity)
{
    if (!engine) return -1;
    int ent = entity;
    typedef void* (*GetPlayerOwned)(void*, const int*);
    void* po = ((GetPlayerOwned)(g_base + RVA_GET_PLAYEROWNED))(engine, &ent);
    if (po) { InterlockedIncrement(&g_siDirect); return *(const int*)po; }
    const void* desc = (const void*)(g_base + RVA_TI_STATIONGROUP);
    typedef int (*GetTypeIndex)(void*, const void**);
    const int ti = ((GetTypeIndex)(g_base + RVA_GET_TYPEINDEX))(engine + 0x48, &desc);
    if (ti < 0) return -1;
    const int slot = TrainOrderSlot(engine, entity, ti);   // -1 = not a station group (a town, an industry)
    if (slot < 0) {
        InterlockedIncrement(&g_siNoOwner);
        if (InterlockedIncrement(&g_siNoOwnerShown) <= 6)
            Log("[stationicon] entity %d: no PlayerOwned and no StationGroup (ti=%d) -- a town/industry/building, untinted\n", entity, ti);
        return -1;
    }
    const uint8_t* comp = EcsComponentAt(engine, ti, slot, STATIONGROUP_STRIDE);
    if (!comp || !Readable(comp, 16)) return -1;
    const int* begin = *(const int* const*)(comp + 0);
    const int* end = *(const int* const*)(comp + 8);
    if (!begin || end <= begin || !Readable(begin, 4)) return -1;   // no stations yet
    int station0 = begin[0];
    void* po2 = ((GetPlayerOwned)(g_base + RVA_GET_PLAYEROWNED))(engine, &station0);
    if (!po2) {
        InterlockedIncrement(&g_siNoOwner);
        if (InterlockedIncrement(&g_siNoOwnerShown) <= 6)
            Log("[stationicon] entity %d is a StationGroup (ti=%d slot=%d) but its first station %d has no PlayerOwned\n", entity, ti, slot, station0);
        return -1;
    }
    InterlockedIncrement(&g_siWalked);
    return *(const int*)po2;
}

// The ROOT tag (button root, 0x5e38e1). Kept -- it is what the ancestor-selector
// rules key on -- but measured not to restyle children created before it, so the
// visible tint comes from IconClassApply below.
extern "C" void StationIconTint(void* component, int entity)
{
    InterlockedIncrement(&g_siAsked);
    __try {
        if (!component) return;
        const int owner = IconOwnerForEntity(IconEngineRecent(2000), entity);   // this build's constructor derived it
        if (owner < 0) return;
        const int cid = IconCompanyOfPid(owner);
        if (cid <= 0) return;
        TintApplyClass(component, cid, "stationicon", entity, owner, &g_siShown);
        InterlockedIncrement(&g_siTinted);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_siFaults); }
}

// ---- THE CLASS ON THE ICON ITSELF (2026-09-16, third try) ----
// Tagging the root landed (read back) but painted nothing: the game styles a
// component when it is created, and StationItem's ::StationIcon child (the
// box-and-glyph image, hud.lua) is created inside the content build -- BEFORE
// the root gets our class -- so an ancestor rule never re-resolves it. The
// game's own variants put the class on the icon element itself
// (StationItem::StationIcon!train, !hover), applied by addStyleClass 0x227a1e0
// at 0x5e07f9 (StationItem) and 0x5e2d13 (VehicleDepotItem), rcx = the icon
// component, rdx = the carrier class string. Those two calls now go through
// IconClassApply: the original, then "!mpWinCoN" on the SAME component, whose
// entity the content-builder entry hook (0x5e45d0, pre-hook) recorded. Rule:
// "StationItem::StationIcon!mpWinCoN" { backgroundColor1 = colour } in the mod
// sheet, the same grammar as !train.
static volatile LONG g_curIconEntity = -1;   // set at 0x5e45d0 entry (edx), main thread, serial
static const uintptr_t RVA_ICON_CONTENT_FN   = 0x5e45d0;   // FUN_5e45d0(context, entity): builds the item content
static const uint8_t ICON_CONTENT_PROLOGUE[15] = {
    0x89, 0x54, 0x24, 0x10,              // mov [rsp+0x10], edx
    0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,   // push rbx/rsi/rdi/r14/r15
    0x48, 0x83, 0xEC, 0x70               // sub rsp, 0x70
};
// THE REBUILD PATH (2026-09-16, fourth try). CreateStationGroupItem2 (0x5dfcf0,
// called by the content builder) wraps the StationItem in a ContentView and arms
// a 1000 ms re-evaluation (0x227ca40) whose callback (0x5e5970, no static
// caller) constructs a FRESH StationItem (0x5e0070) and swaps it in
// (setContent 0x2286020) whenever the waiting-cargo state changes. That path
// never enters the content builder, so g_curIconEntity was whatever the LAST
// DoStep build recorded: the rebuilt icon got another entity's owner (a town ->
// vanilla, another station -> the wrong colour, "two colours"). The constructor
// is common to both paths and takes the entity as its 6th argument ([rsp+0x30]
// at entry), so its entry pre-hook records the right entity every time.
static const uintptr_t RVA_STNITEM_CTOR = 0x5e0070;          // StationItem(ctx, a, b, c, sys, entity, i, i, i, cfg)
static const uint8_t STNITEM_CTOR_PROLOGUE[15] = {
    0x48, 0x8B, 0xC4,                    // mov rax, rsp
    0x4C, 0x89, 0x48, 0x20,              // mov [rax+0x20], r9
    0x4C, 0x89, 0x40, 0x18,              // mov [rax+0x18], r8
    0x48, 0x89, 0x50, 0x10               // mov [rax+0x10], rdx
};
static const uint32_t STNITEM_CTOR_ENTITY_ARG = 0x30;        // [rsp+0x30] at entry = the 6th argument, ecs::Entity
// VehicleDepotItem(EnginePtr rcx, entity edx, int r8d, ...): it hands &rcx-home to
// EngineFromPtr itself (lea rcx,[rbp+0x67]; call 0x8b9e60), so rcx IS the EnginePtr.
static const uintptr_t RVA_DEPOTITEM_CTOR = 0x5e2b70;
static const uint8_t DEPOTITEM_CTOR_PROLOGUE[9] = {
    0x89, 0x54, 0x24, 0x10,              // mov [rsp+0x10], edx
    0x48, 0x89, 0x4C, 0x24, 0x08         // mov [rsp+8], rcx
};
static const uintptr_t RVA_STNICON_CLASS_CALL  = 0x5e07f9;  // call 0x227a1e0 in StationItem (rcx = ::StationIcon)
static const uintptr_t RVA_DEPOTICON_CLASS_CALL = 0x5e2d13; // call 0x227a1e0 in VehicleDepotItem (rcx = ::Icon)
static const uint8_t STNICON_CLASS_EXPECT[5]   = { 0xE8, 0xE2, 0x99, 0xC9, 0x01 };
static const uint8_t DEPOTICON_CLASS_EXPECT[5] = { 0xE8, 0xC8, 0x74, 0xC9, 0x01 };
static volatile LONG g_icApplied = 0, g_icShown = 0;
// THE POST-ATTACH RESTYLE (2026-09-16, fifth try). Measured: an icon tagged in
// its constructor shows the colour when the 1000 ms cargo rebuild swaps it into
// the already-attached ContentView, but NOT when DoStep builds it fresh -- the
// tag lands before the button is attached to the HUD layer and the engine only
// honours it at a restyle after that (hover, zoom). So the icon component
// tagged during THIS build is remembered and, right after DoStep hands the
// button to the layer (call 0x224a920 at 0x5e3add), the class is added again on
// the now-attached element: the same post-attach addStyleClass hover does.
// Same thread, same DoStep iteration, so the pointer is live; the entity check
// keeps a rebuild-path tag (no attach hook) from being replayed on a later build.
static void* volatile g_lastIconComp = nullptr;
static volatile LONG g_lastIconEntity = -1, g_lastIconCid = 0;
static volatile LONG g_iaApplied = 0, g_iaShown = 0;
static const uintptr_t RVA_ICON_ATTACH_HOOK = 0x5e3ae2;   // right after `call 0x224a920` (the layer takes the button)
static const uint8_t ICON_ATTACH_EXPECT[8] = {
    0x48, 0x8B, 0x45, 0x80,        // mov rax, [rbp-0x80]
    0x48, 0x8B, 0x58, 0x18         // mov rbx, [rax+0x18]
};

static void IconAttachedApply(void* comp, int cid, int entity)   // the std::string lives here, outside __try (C2712)
{
    // NOT the company class again: addStyleClass drops a duplicate (read back: the list
    // stays "road mpWinCo2") and a dropped duplicate restyles nothing. A class the
    // element does not have yet is a real change, and the restyle it triggers
    // re-resolves the whole list, company class included (what !hover does).
    (void)cid;
    std::string cls = "mpAttached";
    typedef void (*AddClass)(void*, const void*);
    ((AddClass)(g_base + RVA_ADD_STYLE_CLASS))(comp, &cls);
    InterlockedIncrement(&g_iaApplied);
    if (InterlockedIncrement(&g_iaShown) <= 4) {
        char list[512];
        TintClassList(comp, list, sizeof(list));
        Log("[stationicon-attach] entity %d: added mpAttached after the HUD layer took the button; classes now: %s\n", entity, list);
    }
}

extern "C" void IconAttached()
{
    __try {
        void* comp = g_lastIconComp;
        if (!comp) return;
        const int entity = (int)InterlockedCompareExchange(&g_lastIconEntity, 0, 0);
        if (entity != (int)InterlockedCompareExchange(&g_curIconEntity, 0, 0)) return;
        g_lastIconComp = nullptr;
        const int cid = (int)InterlockedCompareExchange(&g_lastIconCid, 0, 0);
        if (cid <= 0) return;
        IconAttachedApply(comp, cid, entity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_siFaults); }
}

extern "C" void IconClassApply(void* comp, const void* cls)
{
    typedef void (*AddClass)(void*, const void*);
    ((AddClass)(g_base + RVA_ADD_STYLE_CLASS))(comp, cls);      // the game's own carrier class first
    __try {
        const int entity = (int)InterlockedCompareExchange(&g_curIconEntity, 0, 0);
        if (entity < 0 || !comp) return;
        const int owner = IconOwnerForEntity(IconEngineNow(), entity);      // the constructor's own EnginePtr, live now
        if (owner < 0) return;
        const int cid = IconCompanyOfPid(owner);
        if (cid <= 0) return;
        TintApplyClass(comp, cid, "stationicon-glyph", entity, owner, &g_icShown);
        InterlockedIncrement(&g_icApplied);
        g_lastIconCid = cid;
        g_lastIconEntity = entity;
        g_lastIconComp = comp;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_siFaults); }
}

// Rewrites one `call 0x227a1e0` to call IconClassApply through a near jmp-stub
// (the DLL may sit beyond rel32 reach); the caller's return address is untouched.
static bool RedirectClassCall(uintptr_t siteRva, const uint8_t* expect, const char* what)
{
    if (!BytesAre(siteRva, expect, 5, "stationicon")) return false;
    int32_t rel = 0; memcpy(&rel, expect + 1, 4);
    if ((uintptr_t)((int64_t)siteRva + 5 + rel) != RVA_ADD_STYLE_CLASS) {
        Log("[stationicon] NOT installed: %s call at rva=%llx does not resolve to addStyleClass\n", what, (unsigned long long)siteRva);
        return false;
    }
    uint8_t* stub = NearAlloc(16);
    if (!stub) return false;
    const uintptr_t fn = (uintptr_t)&IconClassApply;
    stub[0] = 0xFF; stub[1] = 0x25; memset(stub + 2, 0, 4); memcpy(stub + 6, &fn, 8);   // jmp [rip+0] -> IconClassApply
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
    const uintptr_t at = g_base + siteRva;
    const int64_t nrel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) return false;
    DWORD old = 0;
    if (!VirtualProtect((void*)(at + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t r32 = (int32_t)nrel;
    memcpy((void*)(at + 1), &r32, 4);
    VirtualProtect((void*)(at + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return true;
}

static void InstallIconClassApply()
{
    if (FlagsSayOff("stationicon")) return;
    if (!BytesAre(RVA_ICON_CONTENT_FN, ICON_CONTENT_PROLOGUE, sizeof(ICON_CONTENT_PROLOGUE), "stationicon")) return;
    // the entry pre-hook: record edx (the entity), run the stolen prologue, continue
    uint8_t* stub = NearAlloc(32);
    if (!stub) return;
    void* tramp = nullptr;
    const uintptr_t slot = (uintptr_t)&g_curIconEntity;
    size_t k = 0;
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &slot, 8); k += 8;   // mov rax, &g_curIconEntity
    stub[k++] = 0x89; stub[k++] = 0x10;                                       // mov [rax], edx
    const size_t jmpAt = k;
    stub[k++] = 0x48; stub[k++] = 0xB8; memset(stub + k, 0, 8); k += 8;       // mov rax, <tramp>
    stub[k++] = 0xFF; stub[k++] = 0xE0;                                       // jmp rax
    if (!PatchJumpNear(g_base + RVA_ICON_CONTENT_FN, stub, sizeof(ICON_CONTENT_PROLOGUE), &tramp) || !tramp) {
        Log("[stationicon] NOT installed: could not detour the content builder at rva=%llx\n", (unsigned long long)RVA_ICON_CONTENT_FN);
        return;
    }
    const uintptr_t tp = (uintptr_t)tramp;
    DWORD old = 0;
    VirtualProtect(stub, 32, PAGE_EXECUTE_READWRITE, &old);
    memcpy(stub + jmpAt + 2, &tp, 8);
    VirtualProtect(stub, 32, old, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    const bool s1 = RedirectClassCall(RVA_STNICON_CLASS_CALL, STNICON_CLASS_EXPECT, "StationItem::StationIcon");
    const bool s2 = RedirectClassCall(RVA_DEPOTICON_CLASS_CALL, DEPOTICON_CLASS_EXPECT, "VehicleDepotItem::Icon");
    // the StationItem constructor entry: record its entity argument (covers the
    // 1000 ms cargo-state rebuild, which never passes the content builder)
    bool s3 = false;
    if (BytesAre(RVA_STNITEM_CTOR, STNITEM_CTOR_PROLOGUE, sizeof(STNITEM_CTOR_PROLOGUE), "stationicon")) {
        uint8_t* cs = NearAlloc(64);
        void* ctramp = nullptr;
        if (cs) {
            size_t j = 0;
            cs[j++] = 0x49; cs[j++] = 0xBA; memcpy(cs + j, &slot, 8); j += 8;              // mov r10, &g_curIconEntity
            cs[j++] = 0x8B; cs[j++] = 0x44; cs[j++] = 0x24; cs[j++] = (uint8_t)STNITEM_CTOR_ENTITY_ARG; // mov eax, [rsp+0x30]
            cs[j++] = 0x41; cs[j++] = 0x89; cs[j++] = 0x02;                                // mov [r10], eax
            const uintptr_t eslot = (uintptr_t)&g_curIconEnginePtr;
            cs[j++] = 0x49; cs[j++] = 0xBB; memcpy(cs + j, &eslot, 8); j += 8;             // mov r11, &g_curIconEnginePtr
            cs[j++] = 0x49; cs[j++] = 0x89; cs[j++] = 0x13;                                // mov [r11], rdx  (the EnginePtr)
            const size_t cj = j;
            cs[j++] = 0x48; cs[j++] = 0xB8; memset(cs + j, 0, 8); j += 8;                  // mov rax, <tramp>
            cs[j++] = 0xFF; cs[j++] = 0xE0;                                                // jmp rax
            if (PatchJumpNear(g_base + RVA_STNITEM_CTOR, cs, sizeof(STNITEM_CTOR_PROLOGUE), &ctramp) && ctramp) {
                const uintptr_t ctp = (uintptr_t)ctramp;
                DWORD o2 = 0;
                VirtualProtect(cs, 64, PAGE_EXECUTE_READWRITE, &o2);
                memcpy(cs + cj + 2, &ctp, 8);
                VirtualProtect(cs, 64, o2, &o2);
                FlushInstructionCache(GetCurrentProcess(), cs, 64);
                s3 = true;
            } else {
                Log("[stationicon] NOT installed: could not detour the StationItem constructor at rva=%llx\n", (unsigned long long)RVA_STNITEM_CTOR);
            }
        }
    }
    // the post-attach restyle: after `call 0x224a920` in DoStep. rax/rcx/rdx/r8-r11
    // are dead there (rax is reloaded by the first stolen instruction); align, call,
    // re-run the two stolen loads, resume at hook+8.
    bool s4 = false;
    if (BytesAre(RVA_ICON_ATTACH_HOOK, ICON_ATTACH_EXPECT, sizeof(ICON_ATTACH_EXPECT), "stationicon")) {
        uint8_t* as = NearAlloc(64);
        if (as) {
            const uintptr_t helper = (uintptr_t)&IconAttached;
            size_t j = 0;
            as[j++] = 0x55;                                                        // push rbp
            as[j++] = 0x48; as[j++] = 0x8B; as[j++] = 0xEC;                        // mov rbp, rsp
            as[j++] = 0x48; as[j++] = 0x83; as[j++] = 0xE4; as[j++] = 0xF0;        // and rsp, -16
            as[j++] = 0x48; as[j++] = 0x83; as[j++] = 0xEC; as[j++] = 0x20;        // sub rsp, 0x20
            as[j++] = 0x48; as[j++] = 0xB8; memcpy(as + j, &helper, 8); j += 8;    // mov rax, IconAttached
            as[j++] = 0xFF; as[j++] = 0xD0;                                        // call rax
            as[j++] = 0x48; as[j++] = 0x8B; as[j++] = 0xE5;                        // mov rsp, rbp
            as[j++] = 0x5D;                                                        // pop rbp
            memcpy(as + j, ICON_ATTACH_EXPECT, sizeof(ICON_ATTACH_EXPECT)); j += sizeof(ICON_ATTACH_EXPECT);   // the stolen loads
            const uintptr_t resume = g_base + RVA_ICON_ATTACH_HOOK + sizeof(ICON_ATTACH_EXPECT);
            as[j++] = 0xE9;
            const int32_t rel = (int32_t)((int64_t)resume - (int64_t)((uintptr_t)as + j + 4));
            memcpy(as + j, &rel, 4); j += 4;
            FlushInstructionCache(GetCurrentProcess(), as, j);
            const uintptr_t at = g_base + RVA_ICON_ATTACH_HOOK;
            const int64_t nrel = (int64_t)(uintptr_t)as - (int64_t)(at + 5);
            DWORD o3 = 0;
            if (nrel >= INT32_MIN && nrel <= INT32_MAX && VirtualProtect((void*)at, 8, PAGE_EXECUTE_READWRITE, &o3)) {
                uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
                const int32_t r32 = (int32_t)nrel;
                memcpy(patch + 1, &r32, 4);
                memcpy((void*)at, patch, 8);
                VirtualProtect((void*)at, 8, o3, &o3);
                FlushInstructionCache(GetCurrentProcess(), (void*)at, 8);
                s4 = true;
            } else {
                Log("[stationicon] NOT installed: could not patch the attach site at rva=%llx\n", (unsigned long long)RVA_ICON_ATTACH_HOOK);
            }
        }
    }
    // the depot item constructor: rcx = EnginePtr, edx = entity (steal 9, no relative operands)
    bool s5 = false;
    if (BytesAre(RVA_DEPOTITEM_CTOR, DEPOTITEM_CTOR_PROLOGUE, sizeof(DEPOTITEM_CTOR_PROLOGUE), "stationicon")) {
        uint8_t* ds = NearAlloc(64);
        void* dtramp = nullptr;
        if (ds) {
            size_t j = 0;
            ds[j++] = 0x49; ds[j++] = 0xBA; memcpy(ds + j, &slot, 8); j += 8;              // mov r10, &g_curIconEntity
            ds[j++] = 0x41; ds[j++] = 0x89; ds[j++] = 0x12;                                // mov [r10], edx
            const uintptr_t eslot2 = (uintptr_t)&g_curIconEnginePtr;
            ds[j++] = 0x49; ds[j++] = 0xBB; memcpy(ds + j, &eslot2, 8); j += 8;            // mov r11, &g_curIconEnginePtr
            ds[j++] = 0x49; ds[j++] = 0x89; ds[j++] = 0x0B;                                // mov [r11], rcx  (the EnginePtr)
            const size_t dj = j;
            ds[j++] = 0x48; ds[j++] = 0xB8; memset(ds + j, 0, 8); j += 8;                  // mov rax, <tramp>
            ds[j++] = 0xFF; ds[j++] = 0xE0;                                                // jmp rax
            if (PatchJumpNear(g_base + RVA_DEPOTITEM_CTOR, ds, sizeof(DEPOTITEM_CTOR_PROLOGUE), &dtramp) && dtramp) {
                const uintptr_t dtp = (uintptr_t)dtramp;
                DWORD o4 = 0;
                VirtualProtect(ds, 64, PAGE_EXECUTE_READWRITE, &o4);
                memcpy(ds + dj + 2, &dtp, 8);
                VirtualProtect(ds, 64, o4, &o4);
                FlushInstructionCache(GetCurrentProcess(), ds, 64);
                s5 = true;
            } else {
                Log("[stationicon] NOT installed: could not detour the VehicleDepotItem constructor at rva=%llx\n", (unsigned long long)RVA_DEPOTITEM_CTOR);
            }
        }
    }
    Log("[stationicon] glyph class: content-builder entry hooked at rva=%llx; StationItem ctor entry %s; depot ctor entry %s; post-attach restyle %s; StationIcon call %s, depot Icon call %s\n",
        (unsigned long long)RVA_ICON_CONTENT_FN, s3 ? "hooked (rebuild path covered)" : "NOT hooked (cargo rebuilds keep a stale entity)",
        s5 ? "hooked (its own EnginePtr)" : "NOT hooked (depots take the last station's engine)",
        s4 ? "hooked" : "NOT hooked (fresh icons colour only after a restyle)",
        s1 ? "redirected" : "NOT redirected", s2 ? "redirected" : "NOT redirected");
}

static void InstallStationIconColor()
{
    if (FlagsSayOff("stationicon")) {
        Log("[stationicon] OFF (stationicon=0 in tpf2_menu_flags.txt) -- HUD station/depot icons "
            "are not washed with the owner's colour\n");
        return;
    }
    if (!BytesAre(RVA_ICON_STN_HOOK, ICON_STN_EXPECT, sizeof(ICON_STN_EXPECT), "stationicon")) return;
    // Post-call hook, right after the ItemButton wrap: rax = the button ROOT (the
    // component the HUD places and paints), ebx = entity. Preserve rax across the
    // tint call (the stolen `mov rdi,rax` needs it), align rsp, then re-run the two
    // stolen instructions and resume at hook+6. ebx is nonvolatile (kept by the C fn).
    uint8_t* stub = NearAlloc(96);
    if (!stub) { Log("[stationicon] NOT installed: no page for the stub\n"); return; }
    const uintptr_t helper = (uintptr_t)&StationIconTint;
    size_t k = 0;
    stub[k++] = 0x56;                                                        // push rsi (save DoStep's)
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xF0;                    // mov rsi, rax  (component, survives call)
    stub[k++] = 0x55;                                                        // push rbp
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xEC;                    // mov rbp, rsp
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xE4; stub[k++] = 0xF0;  // and rsp, -16
    stub[k++] = 0x48; stub[k++] = 0x83; stub[k++] = 0xEC; stub[k++] = 0x20;  // sub rsp, 0x20
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xCE;                    // mov rcx, rsi  (component)
    stub[k++] = 0x8B; stub[k++] = 0xD3;                                      // mov edx, ebx  (entity)
    stub[k++] = 0x48; stub[k++] = 0xB8; memcpy(stub + k, &helper, 8); k += 8;// mov rax, StationIconTint
    stub[k++] = 0xFF; stub[k++] = 0xD0;                                      // call rax
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xE5;                    // mov rsp, rbp
    stub[k++] = 0x5D;                                                        // pop rbp
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xC6;                    // mov rax, rsi  (component back)
    stub[k++] = 0x5E;                                                        // pop rsi
    stub[k++] = 0x48; stub[k++] = 0x8B; stub[k++] = 0xF8;                    // mov rdi, rax     (stolen)
    stub[k++] = 0x45; stub[k++] = 0x33; stub[k++] = 0xE4;                    // xor r12d, r12d   (stolen)
    // jmp hook+6 (rel32; keeps rax = component)
    const uintptr_t resume = g_base + RVA_ICON_STN_HOOK + 6;
    stub[k++] = 0xE9;
    const int32_t rel = (int32_t)((int64_t)resume - (int64_t)((uintptr_t)stub + k + 4));
    memcpy(stub + k, &rel, 4); k += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, k);
    // write E9 rel32 at the hook -> stub (steal 6, pad 1 with nop)
    const uintptr_t at = g_base + RVA_ICON_STN_HOOK;
    const int64_t nrel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) { Log("[stationicon] NOT installed: stub out of reach\n"); return; }
    DWORD old = 0;
    if (!VirtualProtect((void*)at, 6, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[stationicon] NOT installed: could not unprotect rva=%llx\n", (unsigned long long)RVA_ICON_STN_HOOK);
        return;
    }
    uint8_t patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    const int32_t r32 = (int32_t)nrel;
    memcpy(patch + 1, &r32, 4);
    memcpy((void*)at, patch, 6);
    VirtualProtect((void*)at, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 6);
    g_stnIconColorOn = true;
    Log("[stationicon] installed: HUD station/depot icons washed the owner's company colour "
        "(hook at rva=%llx)\n", (unsigned long long)RVA_ICON_STN_HOOK);
}

static void InstallSharedStations()
{
    if (FlagsSayOff("sharedstations")) {
        Log("[sharedstations] OFF (sharedstations=0 in tpf2_menu_flags.txt) -- the line editor "
            "keeps refusing another company's stations\n");
        return;
    }
    if (!BytesAre(RVA_SHAREDSTATIONS_GUARD, SHAREDSTATIONS_EXPECT,
                  sizeof(SHAREDSTATIONS_EXPECT), "sharedstations")) return;
    // Both rel32 calls inside the guarded window must resolve where the finding
    // says: a byte match that landed on some other function cannot pass.
    int32_t rel = 0;
    memcpy(&rel, SHAREDSTATIONS_EXPECT + 5, 4);
    uintptr_t target = (uintptr_t)((int64_t)RVA_SHAREDSTATIONS_GUARD + 9 + rel);
    if (target != RVA_SS_GETENGINE) {
        Log("[sharedstations] NOT installed: the call at rva=%llx resolves to %llx, not the "
            "engine accessor %llx\n", (unsigned long long)(RVA_SHAREDSTATIONS_GUARD + 4),
            (unsigned long long)target, (unsigned long long)RVA_SS_GETENGINE);
        return;
    }
    memcpy(&rel, SHAREDSTATIONS_EXPECT + 18, 4);
    target = (uintptr_t)((int64_t)RVA_SHAREDSTATIONS_GUARD + 22 + rel);
    if (target != RVA_SS_GETPLAYEROWNED) {
        Log("[sharedstations] NOT installed: the call at rva=%llx resolves to %llx, not "
            "GetComponentPtr<PlayerOwned> %llx\n",
            (unsigned long long)(RVA_SHAREDSTATIONS_GUARD + 17),
            (unsigned long long)target, (unsigned long long)RVA_SS_GETPLAYEROWNED);
        return;
    }
    // The stub, assembled here rather than in MASM because it is a jump target
    // in the middle of a function, not a call: no prologue, no frame, and it
    // must leave rbx and rsp exactly as it found them.
    //   mov  ecx, eax              ; PlayerOwned.player
    //   mov  edx, [rbx+0x28]       ; the filter's own player entity
    //   sub  rsp, 0x20             ; shadow space; rsp was 16-aligned here
    //   mov  rax, SharedStationsAllow ; call rax ; add rsp, 0x20
    //   test al, al ; jne accept
    //   mov  rax, reject ; jmp rax
    // accept:
    //   mov  rax, accept ; jmp rax
    uint8_t* stub = NearAlloc(64);
    if (!stub) {
        Log("[sharedstations] NOT installed: no page within reach of a rel32 for the stub\n");
        return;
    }
    size_t n = 0;
    const uintptr_t helper = (uintptr_t)&SharedStationsAllow;
    const uintptr_t accept = g_base + RVA_SHAREDSTATIONS_ACCEPT;
    const uintptr_t reject = g_base + RVA_SHAREDSTATIONS_REJECT;
    stub[n++] = 0x8B; stub[n++] = 0xC8;                                      // mov ecx, eax
    stub[n++] = 0x8B; stub[n++] = 0x53; stub[n++] = 0x28;                    // mov edx,[rbx+0x28]
    stub[n++] = 0x48; stub[n++] = 0x83; stub[n++] = 0xEC; stub[n++] = 0x20;  // sub rsp, 0x20
    stub[n++] = 0x48; stub[n++] = 0xB8; memcpy(stub + n, &helper, 8); n += 8;// mov rax, helper
    stub[n++] = 0xFF; stub[n++] = 0xD0;                                      // call rax
    stub[n++] = 0x48; stub[n++] = 0x83; stub[n++] = 0xC4; stub[n++] = 0x20;  // add rsp, 0x20
    stub[n++] = 0x84; stub[n++] = 0xC0;                                      // test al, al
    stub[n++] = 0x75; stub[n++] = 0x0C;                                      // jne +12 (accept)
    stub[n++] = 0x48; stub[n++] = 0xB8; memcpy(stub + n, &reject, 8); n += 8;// mov rax, reject
    stub[n++] = 0xFF; stub[n++] = 0xE0;                                      // jmp rax
    stub[n++] = 0x48; stub[n++] = 0xB8; memcpy(stub + n, &accept, 8); n += 8;// mov rax, accept
    stub[n++] = 0xFF; stub[n++] = 0xE0;                                      // jmp rax
    FlushInstructionCache(GetCurrentProcess(), stub, n);
    if (!PatchJumpNear(g_base + RVA_SHAREDSTATIONS_SITE, stub, SHAREDSTATIONS_STEAL, nullptr)) {
        Log("[sharedstations] NOT installed: could not write the detour at rva=%llx\n",
            (unsigned long long)RVA_SHAREDSTATIONS_SITE);
        return;
    }
    g_ssOn = true;
    Log("[sharedstations] installed rva=%llx steal=%d accept=%llx reject=%llx -- in companies "
        "mode the line editor takes another company's station as a stop; ownership, edits and "
        "demolition are unchanged\n", (unsigned long long)RVA_SHAREDSTATIONS_SITE,
        SHAREDSTATIONS_STEAL, (unsigned long long)RVA_SHAREDSTATIONS_ACCEPT,
        (unsigned long long)RVA_SHAREDSTATIONS_REJECT);
}

// ---------------------------------------------------------------------------
// SHIP AND AIRCRAFT CLAIM ORDER -- measured, not enforced, and here is why.
//
// THE FINDING (RE pass on build 35924). ecs::ShipMoveSystem::Update2 (0xa6c1e0,
// 10,477 bytes) and ecs::AircraftMoveSystem::Update2 (0xa2bc60, 14,604 bytes)
// are both entirely serial and both walk their ECS family's node vector
// linearly -- ship `0xa6c43a mov rax,[r13+8]; mov rsi,[rax]; add rsi,r12` with
// `add r12,0x14` at 0xa6c7e9 and the count in [rbp+0x20]; aircraft the same
// shape at 0xa2beb3 with `add rdx,0x14` at 0xa2c434 and the count in
// [rbp-0x28]. Arbitration is check-then-claim against a persistent map: `call
// IsReserved (0x2114e70)` then, on je, `call Reserve (0x21150b0)` -- ship at
// 0xa6c610/0xa6c650, aircraft at 0xa2c085/0xa2c0c5, with further Reserve sites
// at 0xa2c1f0, 0xa2c3d8 and the runway one at 0xa2f0f4. There is no shuffle
// anywhere in either. The first vehicle in NODE-LIST order to ask for a
// contended lock gets it -- and node-list order is the order the engine
// registered those entities in, which is not replicated state. Two peers
// holding the same world, agreeing on the hash, can send different ships
// through the same lock.
//
// So far this is the train bug. It is not fixed the same way, and that is a
// deliberate decision:
//
// WHAT THE TRAIN PATCH REORDERS, AND WHY THIS CANNOT. TrainMoveSystem::Update2
// builds idx[] = iota over its nodes and shuffles THAT. The train patch sorts a
// scratch array the engine made three instructions earlier, inside the frame it
// was made in, and nothing else in the process can see it. Ship and aircraft
// Update2 have no such array. They index the family node vector directly, so
// the only thing there is to reorder is the ecs::NodeList<4> itself -- the
// caller (ShipMoveSystem::Update, 0xa6ead0; AircraftMoveSystem::Update,
// 0xa2f570) downcasts the INodeList it is handed, points this+8 at its vector,
// derives n from the vector's byte span (/20) and calls Update2 through the
// vtable -- engine-owned, alive across frames, and shared with whatever else
// reads that family. Three things in the disassembly argue against permuting
// it blind:
//   1. Update2 calls ecs::Engine::NoteComponentAboutToBeChanged (0xa6c98d ->
//      0x23e0020) while it is iterating, which is exactly the notification
//      that maintains families. A permutation cannot be reliably undone across a call that
//      may have edited what was permuted.
//   2. The engine re-reads the vector's base pointer from [this+8] on EVERY
//      iteration (0xa6c43a, reloading `this` from [rbp-0x48] at 0xa6c7b1)
//      rather than hoisting it -- the shape of code that expects the vector to
//      move underneath it.
//   3. The scratch arrays Update2 fills are indexed by node POSITION (the
//      per-vehicle speed table at [rbp+0x140], written at 0xa6c5ca as
//      [rax+r15*8]), so a permuted vector would need those permuted with it,
//      and the family's own insert/erase path was not found in this pass.
//      "I did not find an index into it" is not "there is no index into it",
//      and the cost of being wrong is memory corruption on every peer at once.
// None of that can be settled by reading; it wants one run with the permutation
// in and an eye on the family. That run is not available here.
//
// WHAT THIS DOES INSTEAD. It measures, in the form two peers can diff. At each
// Update2 it reads the family's node records (20 bytes: entity id, then four
// component indices), reads each vehicle's Name the way the train patch does,
// and logs an FNV over the NAMES in the engine's current node order. Names are
// replicated (VNAME); entity ids are not, by design. So, on two peers' logs:
//
//     same n, same rankHash=, different nameHash=   ->  the two engines hold
//     the same fleet and are about to claim locks in different orders. That is
//     the desync, caught in the act, with no world hash able to see it.
//
// and `reordered=0` on both says the engine happens to be in name order
// already, which is what the eventual enforcement would make permanent. The
// ordering itself (moveorder.h) is the train rule minus the jitter and is
// computed into a PRIVATE index array; the engine's vector is never written.
//
// THE DETOUR. Both Update2s open `mov rax,rsp; push rbp; push rbx` (5 bytes),
// and `mov rax,rsp` has to run with the caller's rsp, so the relay
// (moveorderrelay_slice.asm) saves every register it could disturb, calls
// MoveOrderObserve, restores them, runs those three instructions itself with
// rsp back at the entry value, and jumps to site+5. No trampoline is called.
//
// KILL SWITCHES: `shiporder=0` and `airorder=0` in tpf2_menu_flags.txt.
// ---------------------------------------------------------------------------
#include "moveorder.h"

static const uintptr_t RVA_SHIP_UPDATE2 = 0xa6c1e0;
static const uintptr_t RVA_AIR_UPDATE2  = 0xa2bc60;
static const int       MOVEORDER_STEAL  = 5;      // `mov rax,rsp` + `push rbp` + `push rbx`

// The two prologues, through the frame anchor. They are the same compiler
// template and differ only in the frame size the `lea rbp` carries.
static const uint8_t MOVEORDER_EXPECT_SHIP[22] = {
    0x48, 0x8B, 0xC4,                    // mov rax, rsp          <- the 5 stolen
    0x55, 0x53,                          // push rbp / push rbx
    0x56, 0x57,                          // push rsi / push rdi
    0x41, 0x54, 0x41, 0x55,              // push r12 / push r13
    0x41, 0x56, 0x41, 0x57,              // push r14 / push r15
    0x48, 0x8D, 0xA8, 0xC8, 0xFA, 0xFF, 0xFF   // lea rbp, [rax-0x538]
};
static const uint8_t MOVEORDER_EXPECT_AIR[22] = {
    0x48, 0x8B, 0xC4,
    0x55, 0x53,
    0x56, 0x57,
    0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xA8, 0x78, 0xFA, 0xFF, 0xFF   // lea rbp, [rax-0x588]
};

extern "C" {
    uint64_t g_shipOrderResume = 0;      // site+5, where the relays jump when done
    uint64_t g_airOrderResume = 0;
    void ShipOrderRelay();
    void AirOrderRelay();
}

struct MoveOrderChan {
    const char* tag;
    std::vector<TrainOrderKey> keys;
    std::vector<int32_t> idx;
    volatile LONG busy, calls, refusals, reordered, maxUs, suppressed;
    volatile LONG64 lastN;
    uint32_t lastNameHash, lastIdHash;
    ULONGLONG lastLogMs;
    bool haveLast, on;
};
static MoveOrderChan g_shipChan;
static MoveOrderChan g_airChan;
static const ULONGLONG MOVEORDER_LOG_GAP_MS = 5000;

static void MoveOrderRefuse(MoveOrderChan& ch, const char* why, int64_t n)
{
    static const char* lastRefused[2] = { nullptr, nullptr };
    const int slot = (&ch == &g_shipChan) ? 0 : 1;
    InterlockedIncrement(&ch.refusals);
    if (why == lastRefused[slot]) return;
    lastRefused[slot] = why;
    Log("[%s] refused: %s (n=%lld) -- nothing measured this step\n",
        ch.tag, why, (long long)n);
}

// Read the family, rank it by name into a private index array, and log when
// anything about the answer changed. Never writes to anything the engine owns.
static void MoveOrderMeasure(MoveOrderChan& ch, void* self, void* world, int64_t n)
{
    InterlockedIncrement(&ch.calls);
    InterlockedExchange64(&ch.lastN, (LONG64)n);
    if (n < 0 || n > MOVEORDER_MAX_N) { MoveOrderRefuse(ch, "bad count", n); return; }
    if (n < 2) return;
    if (!self || !Readable((const uint8_t*)self + 8, 8)) {
        MoveOrderRefuse(ch, "no node list", n); return;
    }
    // this+8 points at the NodeList's vector (begin, end): ShipMoveSystem::Update
    // sets it at 0xa6eb35 (`mov [rsi+8],rdi` with rdi = nodelist+8) and derives
    // n from end-begin at 0xa6ec02, so both must agree here.
    uint8_t* holder = *(uint8_t**)((const uint8_t*)self + 8);
    if (!holder || !Readable(holder, 16)) { MoveOrderRefuse(ch, "no node list", n); return; }
    const uint8_t* recs = *(const uint8_t**)holder;
    const uint8_t* recsEnd = *(const uint8_t**)(holder + 8);
    if (!recs || recsEnd < recs || (int64_t)(recsEnd - recs) != n * MOVEORDER_REC) {
        MoveOrderRefuse(ch, "count does not match the node vector", n); return;
    }
    if (!Readable(recs, (size_t)n * MOVEORDER_REC)) {
        MoveOrderRefuse(ch, "records unreadable", n); return;
    }

    if ((int64_t)ch.keys.size() < n) ch.keys.resize((size_t)n);
    if ((int64_t)ch.idx.size() < n) ch.idx.resize((size_t)n);
    TrainOrderKey* keys = ch.keys.data();
    int32_t* idx = ch.idx.data();
    for (int64_t i = 0; i < n; i++) {
        keys[i].name = nullptr; keys[i].len = 0; keys[i].score = 0;
        keys[i].id = MoveOrderRecId(recs, (int32_t)i);
        idx[i] = (int32_t)i;
    }

    uint8_t* w = (uint8_t*)world;
    const bool worldOk = w && Readable(w + 0x48, 8) && Readable(w + 0x88, 8) &&
                         Readable(w + 0xa0, 8) && *(uint8_t**)(w + 0x88) && *(uint8_t**)(w + 0xa0);
    int noName = 0;
    const int typeIdx = worldOk ? TrainOrderNameType(w) : -1;
    if (typeIdx >= 0) {
        for (int64_t i = 0; i < n; i++) {
            const int slot = TrainOrderSlot(w, keys[i].id, typeIdx);
            const uint8_t* comp = slot < 0 ? nullptr : TrainOrderComponent(w, typeIdx, slot);
            if (!comp || !TrainOrderNameText(comp, &keys[i].name, &keys[i].len)) {
                keys[i].name = nullptr; keys[i].len = 0; noName++;
            }
        }
    } else {
        noName = (int)n;
    }

    // Hashed BEFORE the rank, over the iota, so nameHash and idHash describe
    // the engine's own claim order; rankHash is the same names in name order,
    // i.e. the fleet as a set, equal on two peers that hold the same ships.
    const uint32_t nameHash = MoveOrderNameHash(idx, n, keys);
    const uint32_t idHash = MoveOrderIdHash(idx, n, keys);
    const MoveOrderOutcome o = MoveOrderRank(idx, n, keys);
    if (o.refused) { MoveOrderRefuse(ch, o.refused, n); return; }
    const uint32_t rankHash = MoveOrderNameHash(idx, n, keys);
    if (o.changed) InterlockedIncrement(&ch.reordered);

    // First time, and whenever n or either hash moved -- but a fleet whose
    // vector changes shape every step must not turn this into a per-step log,
    // so a change inside the 5 s window is only counted, and the next line
    // past the window says how many steps went unlogged.
    if (ch.haveLast && ch.lastNameHash == nameHash && ch.lastIdHash == idHash &&
        ch.lastN == (LONG64)n) return;
    const ULONGLONG now = GetTickCount64();
    if (ch.haveLast && now - ch.lastLogMs < MOVEORDER_LOG_GAP_MS) {
        InterlockedIncrement(&ch.suppressed);
        return;
    }
    ch.lastNameHash = nameHash; ch.lastIdHash = idHash; ch.haveLast = true;
    ch.lastLogMs = now;
    const LONG skipped = InterlockedExchange(&ch.suppressed, 0);
    Log("[%s] n=%lld named=%lld unnamed=%d nameHash=%08x rankHash=%08x idHash=%08x "
        "reordered=%d suppressed=%ld%s%s\n",
        ch.tag, (long long)n, (long long)o.named, noName, nameHash, rankHash, idHash,
        o.changed ? 1 : 0, skipped,
        o.duplicates ? " DUPLICATE-IDS" : "",
        typeIdx >= 0 ? "" : " NO-NAMES(world unreadable)");
}

// Called by the relays at the first instruction of Update2, before the engine
// has done anything: busy flag, fault guard and the clock, exactly as the train
// patch does it. A fault costs the measurement for one step and nothing else --
// nothing here is written that the engine reads.
extern "C" void MoveOrderObserve(int kind, void* self, void* world, int n)
{
    MoveOrderChan& ch = kind == 0 ? g_shipChan : g_airChan;
    if (InterlockedCompareExchange(&ch.busy, 1, 0) != 0) {
        InterlockedIncrement(&ch.refusals);
        return;
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    __try {
        MoveOrderMeasure(ch, self, world, (int64_t)n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool told[2] = { false, false };
        const int slot = kind == 0 ? 0 : 1;
        if (!told[slot]) {
            told[slot] = true;
            Log("[%s] faulted -- measurement skipped this step, the game is untouched\n", ch.tag);
        }
    }
    InterlockedExchange(&ch.busy, 0);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0) return;
    const LONG us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    if (us > ch.maxUs) {
        InterlockedExchange(&ch.maxUs, us);
        static bool warned[2] = { false, false };
        const int slot = kind == 0 ? 0 : 1;
        if (us > 1000 && !warned[slot]) {
            warned[slot] = true;
            Log("[%s] SLOW: %ld us for %lld vehicles in one step -- over the 1 ms budget\n",
                ch.tag, us, (long long)ch.lastN);
        }
    }
}

static void InstallMoveOrder(MoveOrderChan& ch, const char* tag, uintptr_t rva,
                             const uint8_t* expect, size_t expectLen,
                             void* relay, uint64_t* resume, const char* what)
{
    ch.tag = tag;
    ch.lastN = -1;
    if (FlagsSayOff(tag)) {
        Log("[%s] OFF (%s=0 in tpf2_menu_flags.txt) -- %s claim order is not measured\n",
            tag, tag, what);
        return;
    }
    if (!BytesAre(rva, expect, expectLen, tag)) return;
    *resume = g_base + rva + MOVEORDER_STEAL;
    if (!PatchJumpNear(g_base + rva, relay, MOVEORDER_STEAL, nullptr)) {
        *resume = 0;
        Log("[%s] NOT installed: could not write the detour at rva=%llx\n",
            tag, (unsigned long long)rva);
        return;
    }
    ch.on = true;
    Log("[%s] installed rva=%llx steal=%d resume=%llx -- %s claim order is measured, NOT "
        "changed: the family node vector is engine-owned (see the header)\n",
        tag, (unsigned long long)rva, MOVEORDER_STEAL,
        (unsigned long long)(rva + MOVEORDER_STEAL), what);
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
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, g_dllDir, (int)sizeof(g_dllDir),   // ANSI: fopen() takes it
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
    // KEEP LOGS: while <data dir>\tpf2mp_keep_logs.txt exists, every log that
    // would start afresh is appended to instead (the bridge and menu logs always
    // append); a session banner marks the start. The game's own stdout.txt is
    // the game's -- snapshot it (tools\snapshot_logs.ps1) before a restart.
    char keep[MAX_PATH]; snprintf(keep, sizeof(keep), "%stpf2mp_keep_logs.txt", g_dataDir);
    const bool keepLogs = GetFileAttributesA(keep) != INVALID_FILE_ATTRIBUTES;
    g_log = _fsopen(path, keepLogs ? "a" : "w", _SH_DENYWR);
    if (!g_log) return 0;
    if (keepLogs) { SYSTEMTIME st; GetLocalTime(&st); fprintf(g_log, "\n==== session %04u-%02u-%02u %02u:%02u:%02u pid %lu (tpf2mp_keep_logs.txt present: appending) ====\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId()); }
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
    Log("[slice] attached, base=%llx instance=%s dumpprop=%d\n",
        (unsigned long long)g_base, g_instance[0] ? g_instance : "?",
        DumpPropOn() ? 1 : 0);

    uint8_t* blobs = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blobs) { Log("[slice] blob alloc failed\n"); return 0; }

    // Order matters only for the log; the two targets do not overlap.
    Install(blobs, RVA_BUILDPROPOSAL, STEAL_BUILDPROPOSAL, ID_BUILDPROPOSAL, "BuildProposal");
    Install(blobs + BLOB_SIZE, RVA_CMDADD, STEAL_CMDADD, ID_CMDADD, "CommandList::Add");
    for (int i = 0; i < NUM_FACTORIES; i++)
        Install(blobs + BLOB_SIZE * (2 + i), FACTORIES[i].rva, FACTORIES[i].steal,
                FACTORIES[i].id, FACTORIES[i].name);

    // Not a capture hook: no blob, no trampoline, no relay contract. It only
    // reorders an index array the engine is about to shuffle (see "TRAIN
    // RESERVATION ORDER"), so it installs itself and is independent of the
    // session being live -- two peers have to rank trains the same way from
    // the first sim step, long before anybody clicks anything.
    InstallTrainOrder();
    // The same independence from the session applies to the road sum and the
    // two claim-order observers ("ROAD FREE SPACE", "SHIP AND AIRCRAFT CLAIM
    // ORDER"): the first bus to reach a junction must get the same float on
    // every peer from the first step.
    InstallRoadSpace();
    InstallRoadEntries();
    InstallMoveOrder(g_shipChan, "shiporder", RVA_SHIP_UPDATE2, MOVEORDER_EXPECT_SHIP,
                     sizeof(MOVEORDER_EXPECT_SHIP), (void*)&ShipOrderRelay,
                     &g_shipOrderResume, "ship");
    InstallMoveOrder(g_airChan, "airorder", RVA_AIR_UPDATE2, MOVEORDER_EXPECT_AIR,
                     sizeof(MOVEORDER_EXPECT_AIR), (void*)&AirOrderRelay,
                     &g_airOrderResume, "aircraft");
    // The line editor's owner gate ("SHARED STATIONS"). Installed the same way
    // and for the same reason: it only ever answers a comparison the engine was
    // about to make, and outside companies mode that comparison cannot fail.
    InstallSharedStations();
    // The counter that ticked per render batch while paused ("PAUSED TICK"):
    // the town developer stamps it into every building it proposes, and the
    // account and train systems pick and seed by it.
    InstallPausedTick();
    // Icons over every player's stations and vehicles ("SHOW ALL ICONS"), and
    // the company-colour tint of a foreign vehicle icon ("ICON COLOUR").
    InstallShowAllIcons();
    InstallIconColor();
    // A foreign station's name-label background, washed its owner's colour.
    InstallStationLabelColor();
    // A foreign entity's window opens read-only ("FOREIGN WINDOWS").
    InstallForeignWindows();
    // A foreign entity's window, washed its owner's company colour.
    InstallWindowColor();
    // The HUD station/depot icon, washed its owner's company colour: the root tag,
    // and the class on the icon element itself (what actually paints).
    InstallStationIconColor();
    InstallIconClassApply();

    for (;;) {
        Sleep(15000);
        Log("[slice] alive: captured=%ld cancelled=%ld addHits=%ld\n",
            g_captured, g_suppressed, g_addSeen);
        if (g_trainOrderOn)
            Log("[trainorder] alive: steps=%ld reorders=%ld refused=%ld lastSeed=%lu lastN=%lld maxUs=%ld\n",
                g_toCalls, g_toReorders, g_toRefusals,
                (unsigned long)(ULONG)g_toLastSeed, (long long)g_toLastN, g_toMaxUs);
        if (g_ssOn && g_ssCalls)
            Log("[sharedstations] alive: foreignAsked=%ld opened=%ld refused=%ld\n",
                g_ssCalls, g_ssOpened, g_ssRefused);
        if (g_rsOn)
            Log("[roadspace] alive: calls=%ld filtered=%ld changed=%ld handed=%ld faults=%ld maxN=%ld\n",
                g_rsCallsA, g_rsCallsB, g_rsDiffs, g_rsHanded, g_rsFaults, g_rsMaxN);
        if (g_reOn)
            Log("[roadentries] alive: adds=%ld sorted=%ld refused=%ld faults=%ld maxN=%ld\n",
                g_reCalls, g_reSorted, g_reRefused, g_reFaults, g_reMaxN);
        if (g_stnIconColorOn && g_siAsked)
            Log("[stationicon] alive: asked=%ld direct=%ld walked=%ld tinted=%ld glyphs=%ld noOwner=%ld faults=%ld\n",
                g_siAsked, g_siDirect, g_siWalked, g_siTinted, g_icApplied, g_siNoOwner, g_siFaults);
        if (g_windowColorOn && g_wcAsked)
            Log("[windowcolor] alive: asked=%ld tinted=%ld faults=%ld\n", g_wcAsked, g_wcTinted, g_wcFaults);
        if (g_iaApplied)
            Log("[stationicon-attach] alive: restyled=%ld\n", g_iaApplied);
        if (g_stnLabelColorOn && g_slAsked)
            Log("[stationlabelcolor] alive: labels=%ld tinted=%ld\n", g_slAsked, g_slTinted);
        for (int c = 0; c < 2; c++) {
            const MoveOrderChan& ch = c == 0 ? g_shipChan : g_airChan;
            if (ch.on)
                Log("[%s] alive: steps=%ld refused=%ld reorderedSteps=%ld lastN=%lld maxUs=%ld\n",
                    ch.tag, ch.calls, ch.refusals, ch.reordered, (long long)ch.lastN, ch.maxUs);
        }
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
