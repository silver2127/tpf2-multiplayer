// lines.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// component::Line decoding, the platform assignment at replay, strict line creation and the spare line

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
// LINE PLATFORM ASSIGNMENT AT REPLAY (2026-09-20).
//
// When a station is clicked into a line, the line editor (0x603fa0, and the
// waypoint click 0x603a00) copies the ENGINE's current component::Line, inserts
// the stop, and runs UI::UpdateLineAssignment (0x60a5a0) over every stop:
// line_util::GetBestLineAssignment path-searches from each stop to the next
// through the transport network and picks the terminal that fits the approach
// -- "the best platform". The command carries the result. Under lockstep the
// click's UpdateLine is cancelled and replayed at the stamp, so the engine's
// Line stays stale for the length of the delay; a click inside that window is
// built from a list that lacks the previous stop (the Lua merges the lists
// back, CM.mergeLineEdit) and its platform was chosen against the wrong
// predecessor, or none: the station on a double-track main line came out on
// the wrong side, on every instance alike, and players fixed it by hand.
//
// So the assignment runs AT THE REPLAY instead, on every instance, on the list
// the Lua rebuilt: same step, same network, same input -> same platforms.
//   - 0x60a5a0 is hooked (15-byte prologue steal). The detour records the
//     click it ran for -- entity, the bool it was given, the thread -- and a
//     copy of the MovePathUtilContext the editor built (14 system pointers).
//   - CaptureFactory tags the LUPDATE that follows on the same thread with
//     " asg=<bool>" (LineAssignTagForCapture). The tag is shipped only after
//     the context this DLL builds from the game state (GameStateNow +
//     BuildMovePathCtx) has been compared byte for byte with the editor's copy
//     ONCE on this machine: a layout that does not match is logged and the
//     feature stays off, on every instance alike (nobody assigns).
//   - At the replay the Lua writes lockstep_lassign_<x>.txt ("<line> <bool>
//     <seq>") right before api.cmd.make.updateLine; the factory hook on the Lua
//     path (ApplyLineAssignAtReplay) reads it and runs the game's own routine
//     on the Line the Lua passed, in place, before the command is built.
// The game state: UI::CGameUI+0x450 holds the UI's state provider (CreateUI,
// 0x56a000 `mov rax,[rsi+0x450]`, copied into every component's ptr);
// UI::GameStateProvider's vftable slot 1 (0x8badf0) is `mov rax,[rcx+8]; mov
// rax,[rax+0x158]`: provider+8 is the CGame, CGame+0x158 the state the game
// calls current THIS FRAME. There are TWO: CGame::RunGameSimLoop (0x11875d)
// keeps them at CGame+0x168 -> { GameState* [2], ..., int current at +0x20 }
// and GameState::Replicate copies one into the other every frame, so the
// current one alternates -- the first live test (2026-09-20 16:05) saw every
// replay handed the state of the OTHER engine and gave up. GameState+0x28 is
// the engine (0x8b9e60): the replay takes whichever of the pair holds the
// command's own engine. The CGameUI pointer comes from tpf2_menu.dll's
// per-frame capture (export Tpf2mpGameUi).
// Manual terminal picks (the combo box 0x7b43b0), alternative terminals, stop
// settings and stop removal never ran the assignment, carry no tag, and
// replay verbatim as before.
//
// WHAT THE ASSIGNMENT IS (live + decompiled, 2026-09-20, see
// tools/ghidra/targets/ghidra_targets_lineterminal_notes.txt): section paths
// between consecutive stops over every platform, then the shortest closed
// loop; a loop found gives each stop the loop's platform, none gives platform
// 0. There is no side-of-track preference: on plain double track the loop is
// shortest with platform 0 in both directions, and vanilla picks 0 there too.
// So "changed: none" in the replay log is the normal outcome on such track;
// the pass only moves a platform where signals, one-way track or geometry
// force it -- and there the stamp-time run on the FULL list is what the click
// could not do while the previous stop was still on its way. The engine keeps
// its own Line+0x1c (VehicleInfo) on every update (0x9d9fd0 restores it), so
// nothing about the modes can or need be shipped.
static const uintptr_t RVA_LINE_ASSIGN         = 0x60a5a0;
static const uintptr_t OFF_GAMEUI_STATE_PROVIDER = 0x450;
static const uintptr_t OFF_GAMESTATE_ENGINE    = 0x28;
static const int       LINE_ASSIGN_STEAL       = 15;
static const uint8_t   LINE_ASSIGN_EXPECT[LINE_ASSIGN_STEAL] = {
    0x40, 0x53,                                            // push rbx
    0x48, 0x83, 0xEC, 0x60,                                // sub  rsp, 0x60
    0x48, 0xC7, 0x44, 0x24, 0x40, 0xFE, 0xFF, 0xFF, 0xFF,  // mov  qword [rsp+0x40], -2
};
typedef void (*LineAssignFn)(void* engine, void* ctx, const int32_t* entity, void* line, bool* ok, uint8_t flag);
static LineAssignFn    g_lineAssignTramp = nullptr;
struct MovePathCtx { uint64_t f[14]; };
static volatile LONG   g_asgTid = 0;          // the editor's last assignment: thread,
static volatile LONG   g_asgEntity = 0;       //   line entity,
static volatile LONG   g_asgFlag = 0;         //   the bool it passed,
static volatile LONG64 g_asgAt = 0;           //   when (ms)
static MovePathCtx     g_asgCtx;              //   and the context it built
static volatile LONG   g_asgCtxValid = 0;
static volatile LONG   g_asgLayout = 0;       // 0 unchecked, 1 our context matched the editor's, -1 it did not
static int             g_lineAsgTag = -1;     // the tag for the LUPDATE being written (-1 none)
static long            g_lasgSeen = 0;        // the last lockstep_lassign seq applied
static uint64_t (*g_menuGameUi)() = nullptr;
static bool            g_menuGameUiTried = false;

static void LineAssignDetour(void* engine, void* ctx, const int32_t* entity, void* line, bool* ok, uint8_t flag)
{
    int32_t e = 0;
    __try {
        if (entity) e = *entity;
        if (ctx) { memcpy(&g_asgCtx, ctx, sizeof(g_asgCtx)); InterlockedExchange(&g_asgCtxValid, 1); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { e = 0; }
    InterlockedExchange(&g_asgEntity, e);
    InterlockedExchange(&g_asgFlag, flag ? 1 : 0);
    InterlockedExchange(&g_asgTid, (LONG)GetCurrentThreadId());
    InterlockedExchange64(&g_asgAt, (LONG64)GetTickCount64());
    g_lineAssignTramp(engine, ctx, entity, line, ok, flag);
}

static const uintptr_t OFF_PROVIDER_GAME   = 0x08;    // UI::GameStateProvider+8: the CGame
static const uintptr_t OFF_GAME_STATE_NOW  = 0x158;   // CGame+0x158: this frame's state
static const uintptr_t OFF_GAME_STATE_PAIR = 0x168;   // CGame+0x168 -> { GameState* [2], ... }
static volatile LONG   g_gsSourceLogged = 0;

static bool GameStateHoldsEngine(uint64_t gs, uint64_t engine)
{
    return gs && Readable((void*)(gs + OFF_GAMESTATE_ENGINE), 8) && *(uint64_t*)(gs + OFF_GAMESTATE_ENGINE) == engine;
}

// The game state whose engine is `engine`: this frame's, or its twin.
static uint64_t GameStateNow(uint64_t engine)
{
    if (!g_menuGameUi && !g_menuGameUiTried) {
        g_menuGameUiTried = true;
        HMODULE m = GetModuleHandleA("tpf2_menu.dll");
        if (m) g_menuGameUi = (uint64_t (*)())GetProcAddress(m, "Tpf2mpGameUi");
        if (!g_menuGameUi) Log("[lineassign] tpf2_menu.dll's Tpf2mpGameUi export not found -- no game state, platforms replay as shipped\n");
    }
    if (!g_menuGameUi) return 0;
    const uint64_t ui = g_menuGameUi();
    if (!ui || !Readable((void*)(ui + OFF_GAMEUI_STATE_PROVIDER), 8)) return 0;
    const uint64_t prov = *(uint64_t*)(ui + OFF_GAMEUI_STATE_PROVIDER);
    if (!prov || !Readable((void*)(prov + OFF_PROVIDER_GAME), 8)) return 0;
    const uint64_t game = *(uint64_t*)(prov + OFF_PROVIDER_GAME);
    if (!game || !Readable((void*)(game + OFF_GAME_STATE_PAIR), 8)) return 0;
    uint64_t cand[3] = { 0, 0, 0 };
    const char* src[3] = { "CGame+0x158", "pair[0]", "pair[1]" };
    cand[0] = *(uint64_t*)(game + OFF_GAME_STATE_NOW);
    const uint64_t pair = *(uint64_t*)(game + OFF_GAME_STATE_PAIR);
    if (pair && Readable((void*)pair, 0x28)) { cand[1] = *(uint64_t*)pair; cand[2] = *(uint64_t*)(pair + 8); }
    for (int i = 0; i < 3; i++) {
        if (GameStateHoldsEngine(cand[i], engine)) {
            if (!InterlockedExchange(&g_gsSourceLogged, 1))
                Log("[lineassign] game state for engine %llx is %llx (%s; this frame's %llx, pair %llx/%llx)\n",
                    (unsigned long long)engine, (unsigned long long)cand[i], src[i], (unsigned long long)cand[0],
                    (unsigned long long)cand[1], (unsigned long long)cand[2]);
            return cand[i];
        }
    }
    Log("[lineassign] no game state holds engine %llx (this frame's %llx -> %llx, pair %llx/%llx) -- not used\n",
        (unsigned long long)engine, (unsigned long long)cand[0],
        (unsigned long long)(GameStateHoldsEngine(cand[0], 0) ? 0 : (cand[0] && Readable((void*)(cand[0] + OFF_GAMESTATE_ENGINE), 8) ? *(uint64_t*)(cand[0] + OFF_GAMESTATE_ENGINE) : 0)),
        (unsigned long long)cand[1], (unsigned long long)cand[2]);
    return 0;
}

// The 14 pointers UI::LineEditor copies out of the game state into a
// vehicle_util::MovePathUtilContext (0x603fa0, 0x5fe260: the same list, the
// same order). Verified against the editor's own copy before first use.
static bool BuildMovePathCtx(uint64_t gs, MovePathCtx* c)
{
    if (!Readable((void*)gs, 0x208)) return false;
    const uint64_t h8 = *(uint64_t*)(gs + 0x08);
    if (!h8 || !Readable((void*)h8, 0x98)) return false;
    c->f[0]  = *(uint64_t*)(gs + 0x38);
    c->f[1]  = *(uint64_t*)(h8 + 0x18);
    c->f[2]  = *(uint64_t*)(h8 + 0x90);
    c->f[3]  = *(uint64_t*)(gs + 0x20);
    c->f[4]  = *(uint64_t*)(gs + 0x28);
    c->f[5]  = *(uint64_t*)(gs + 0x138);
    c->f[6]  = *(uint64_t*)(gs + 0x150);
    c->f[7]  = *(uint64_t*)(gs + 0x1a0);
    c->f[8]  = *(uint64_t*)(gs + 0x158);
    c->f[9]  = *(uint64_t*)(gs + 0x160);
    c->f[10] = *(uint64_t*)(gs + 0xb0);
    c->f[11] = *(uint64_t*)(gs + 0x170);
    c->f[12] = *(uint64_t*)(gs + 0x180);
    c->f[13] = *(uint64_t*)(gs + 0x200);
    return true;
}

// The tag for a captured editor UpdateLine: the bool the editor's assignment
// ran with, or -1 when this update did not come out of an assignment (a manual
// terminal pick, a stop setting, a removal) or the feature is off.
static int LineAssignTagForCapture(uint64_t engine, int32_t entity)
{
    if (!g_lineAssignTramp) return -1;
    if ((LONG)GetCurrentThreadId() != g_asgTid || g_asgEntity != entity) return -1;
    const LONG64 age = (LONG64)GetTickCount64() - g_asgAt;
    if (age < 0 || age > 2000) return -1;
    if (g_asgLayout == 0) {
        MovePathCtx mine;
        memset(&mine, 0, sizeof(mine));
        const uint64_t gs = GameStateNow(engine);
        if (gs && g_asgCtxValid && BuildMovePathCtx(gs, &mine) && memcmp(&mine, &g_asgCtx, sizeof(mine)) == 0) {
            InterlockedExchange(&g_asgLayout, 1);
            Log("[lineassign] context verified: the 14 pointers built from game state %llx match the line editor's -- platforms are assigned at the replay from now on\n",
                (unsigned long long)gs);
        } else {
            InterlockedExchange(&g_asgLayout, -1);
            Log("[lineassign] CONTEXT MISMATCH (gs=%llx ctxValid=%ld): built %llx %llx %llx %llx .. editor %llx %llx %llx %llx -- "
                "platform assignment at replay stays OFF (as before this build); report this line\n",
                (unsigned long long)gs, (long)g_asgCtxValid,
                (unsigned long long)mine.f[0], (unsigned long long)mine.f[1], (unsigned long long)mine.f[2], (unsigned long long)mine.f[3],
                (unsigned long long)g_asgCtx.f[0], (unsigned long long)g_asgCtx.f[1], (unsigned long long)g_asgCtx.f[2], (unsigned long long)g_asgCtx.f[3]);
        }
    }
    if (g_asgLayout != 1) return -1;
    return g_asgFlag ? 1 : 0;
}

// The Lua path's make_cmd::UpdateLine: if the Lua asked for it (a fresh
// lockstep_lassign_<x>.txt naming this line), run the game's assignment on the
// Line it passed (r9), in place, before the factory copies it into the command.
static void ApplyLineAssignAtReplay(uint64_t engine, int32_t entity, uint64_t line)
{
    if (!g_lineAssignTramp) return;
    ReadInstance();
    if (!g_instance[0]) return;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_lassign_%s.txt", g_dataDir, g_instance);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) return;
    if (fa.nFileSizeLow == 0) return;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const uint64_t wrote = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    const uint64_t now = ((uint64_t)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    if (now > wrote && now - wrote > 5ULL * 10000000ULL) return;
    FILE* f = _fsopen(p, "r", _SH_DENYNO);
    if (!f) return;
    long lid = 0, flag = 0, seq = 0;
    const int got = fscanf(f, "%ld %ld %ld", &lid, &flag, &seq);
    fclose(f);
    if (got != 3 || lid != (long)entity || seq == g_lasgSeen) return;
    g_lasgSeen = seq;
    const uint64_t gs = GameStateNow(engine);
    MovePathCtx ctx;
    if (!gs || !BuildMovePathCtx(gs, &ctx)) {
        Log("[lineassign] LUPDATE replay line=%d: no game state -- the list is applied as shipped. LOCKSTEP AT RISK if the others assign\n", (int)lid);
        return;
    }
    LineDecode before, after;
    const bool okB = DecodeLine(line, &before);
    int32_t ent = entity;
    g_lineAssignTramp((void*)engine, &ctx, &ent, (void*)line, nullptr, (uint8_t)(flag ? 1 : 0));
    const bool okA = DecodeLine(line, &after);
    char summary[512]; summary[0] = 0; int o = 0;
    if (okB && okA && before.n == after.n) {
        for (int i = 0; i < after.n && o < (int)sizeof(summary) - 24; i++) {
            if (before.st[i].station != after.st[i].station || before.st[i].terminal != after.st[i].terminal)
                o += snprintf(summary + o, sizeof(summary) - o, " %d:%d/%d->%d/%d", i + 1,
                              before.st[i].station, before.st[i].terminal, after.st[i].station, after.st[i].terminal);
        }
    }
    Log("[lineassign] LUPDATE replay line=%d flag=%ld seq=%ld: platforms assigned at the stamp, %d stop(s), changed:%s\n",
        (int)lid, flag, seq, okA ? after.n : -1, summary[0] ? summary : " none");
}

static void InstallLineAssign()
{
    const uint8_t* code = (const uint8_t*)(g_base + RVA_LINE_ASSIGN);
    if (memcmp(code, LINE_ASSIGN_EXPECT, LINE_ASSIGN_STEAL) != 0) {
        Log("[lineassign] NOT installed: prologue at rva=%llx differs from build 35924 -- platforms replay as shipped\n",
            (unsigned long long)RVA_LINE_ASSIGN);
        return;
    }
    if (PrologueSteal(code, 14) != LINE_ASSIGN_STEAL) {
        Log("[lineassign] NOT installed: steal would not land on an instruction boundary\n");
        return;
    }
    void* tramp = nullptr;
    if (!InstallHook(g_base + RVA_LINE_ASSIGN, (void*)&LineAssignDetour, LINE_ASSIGN_STEAL, &tramp)) {
        Log("[lineassign] NOT installed: could not write the detour at rva=%llx\n", (unsigned long long)RVA_LINE_ASSIGN);
        return;
    }
    g_lineAssignTramp = (LineAssignFn)tramp;
    Log("[lineassign] installed rva=%llx steal=%d -- a station clicked into a line gets its platform assigned at the replay, on every instance\n",
        (unsigned long long)RVA_LINE_ASSIGN, LINE_ASSIGN_STEAL);
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

// THE SPARE LINE (2026-09-19, lines.lua CM.spareTick). Holding the callback showed
// the new line 1-2.6 s after the click. The Lua keeps one empty line per player
// pre-made in lockstep (same step, same id everywhere, owned by a hidden pool
// company) and names ours in lockstep_lspare_<x>.txt. A create that is cancelled
// then fires the editor's callback AT ONCE with that line as its result: the
// callbacks (0x6154a0 line manager, 0x610490 line list) only check the command's
// tag (+0xb18 == 3, still true) and read the entity at +0x58 of the command impl,
// then select it. The LCREATEX carries spare=<id>, so the Lua re-owns and re-keys
// that line instead of creating one. The file is consumed here so a second click
// before the Lua provides the next spare falls back to the held callback, and it
// is trusted only while fresh (the Lua rewrites it every few seconds and blanks
// it at boot: a stale id from another world would select a line that is not there).
static const uint64_t LSPARE_MAX_AGE_100NS = 30ULL * 10000000ULL;
static volatile LONG g_lcSpareId = 0;   // the spare this capture will open, 0 = none
static int32_t ReadAndConsumeSpareLine()
{
    ReadInstance();
    if (!g_instance[0]) return 0;
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_lspare_%s.txt", g_dataDir, g_instance);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) return 0;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const uint64_t wrote = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    const uint64_t now = ((uint64_t)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    if (now > wrote && now - wrote > LSPARE_MAX_AGE_100NS) { Log("[slice] CreateLine: the spare file is stale -- not used\n"); return 0; }
    FILE* f = _fsopen(p, "r", _SH_DENYNO);
    if (!f) return 0;
    long id = 0;
    if (fscanf(f, "%ld", &id) != 1) id = 0;
    fclose(f);
    if (id <= 0) return 0;
    // consumed: the Lua writes the next one once it is bound
    FILE* w = _fsopen(p, "w", _SH_DENYNO);
    if (w) fclose(w);
    return (int32_t)id;
}
// FIRING THE EDITOR'S CALLBACK ON THE SPARE (2026-09-19, third attempt). Fired at
// the create's own Add it failed every time, and the reason was not the line
// manager's list: both callbacks (0x6154a0 line manager, 0x610490 line list) hand
// 0x1423e27d0 the COMMAND RESULT's own vector -- begin/end at +8/+0x10 of the
// argument they get, entries of 16 bytes { int32 entity; double gen; int32 x } --
// and require the created entity in it with a generation that still matches the
// registry's (0x1423df380: alive = the slot at [reg+0xa0]+id*24 holds one non-
// negative int, gen = the 12 bytes at [reg+0xb8]+id*12); a miss is an assert. The
// registry is what 0x8b9e60(owner+0x448 | +0x440) returns, owner = the lambda's
// captured manager/list at [impl+8] (thunk 0x618d70 = `add rcx,8; jmp 0x6154a0`;
// 0x610490 is its own _Do_call). After that they open the editor on the entity
// through the registry alone. So the callback is HELD as before and fired from a
// later CommandList::Add on the same (UI) thread -- the GUI state's rename of the
// spare, sent every third frame once the UI's copy of the engine shows the spare
// as the player's, with lockstep_lfire.txt naming it -- with a stand-in result:
// { impl (tag 3 at +0xb18, the spare at +0x58), one entry of the spare with the
// registry's current generation }.
static const uintptr_t RVA_LINEMGR_CB_THUNK = 0x618d70;
static const uintptr_t RVA_LINELIST_CB      = 0x610490;
static const uintptr_t RVA_REGISTRY_OF      = 0x8b9e60;
static const ULONGLONG LSPARE_FIRE_MAX_MS   = 6000;
static volatile LONG g_lcSpareWaitId = 0;      // the spare the held callback should open, 0 = none
static ULONGLONG     g_lcSpareWaitAt = 0;
static DWORD         g_lcUiTid       = 0;      // the thread the create's Add ran on
static void BlankSpareFireFiles()
{
    for (const char* n : { "lockstep_lfire.txt", "lockstep_lfire_req.txt" }) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s%s", g_dataDir, n);
        FILE* w = _fsopen(p, "w", _SH_DENYNO);
        if (w) fclose(w);
    }
}
static int32_t SpareFireGo()
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_lfire.txt", g_dataDir);
    FILE* f = _fsopen(p, "r", _SH_DENYNO);
    if (!f) return 0;
    long id = 0;
    if (fscanf(f, "%ld", &id) != 1) id = 0;
    fclose(f);
    return (int32_t)id;
}
#pragma pack(push, 1)
struct SpareResultEntry { int32_t entity; double gen; int32_t x; };
#pragma pack(pop)
static void TryFireSpareLine()
{
    const int32_t id = (int32_t)InterlockedCompareExchange(&g_lcSpareWaitId, 0, 0);
    if (!id || GetCurrentThreadId() != g_lcUiTid) return;
    const bool late = GetTickCount64() - g_lcSpareWaitAt > LSPARE_FIRE_MAX_MS;
    if (SpareFireGo() != id) {
        if (late) { InterlockedExchange(&g_lcSpareWaitId, 0); BlankSpareFireFiles(); Log("[slice] CreateLine: the GUI never saw spare line %d as the player's -- giving up (the held callback expires)\n", id); }
        return;
    }
    uint8_t* buf = nullptr;
    AcquireSRWLockExclusive(&g_lcLock);
    if (!g_lcStash.empty()) buf = g_lcStash.front().fn;
    ReleaseSRWLockExclusive(&g_lcLock);
    if (!buf) { InterlockedExchange(&g_lcSpareWaitId, 0); BlankSpareFireFiles(); Log("[slice] CreateLine: no held callback for spare line %d -- the editor keeps what it shows\n", id); return; }
    uint64_t impl = 0, vft = 0, doCall = 0, owner = 0;
    memcpy(&impl, buf + 0x38, 8);
    if (!impl || !Readable((void*)impl, 16)) return;
    memcpy(&vft, (void*)impl, 8);
    memcpy(&owner, (void*)(impl + 8), 8);
    if (!vft || !Readable((void*)vft, 8 * 3)) return;
    memcpy(&doCall, (void*)(vft + 0x10), 8);
    size_t regOff = 0;
    if (doCall == (uint64_t)g_base + RVA_LINEMGR_CB_THUNK) regOff = 0x448;
    else if (doCall == (uint64_t)g_base + RVA_LINELIST_CB) regOff = 0x440;
    else {
        InterlockedExchange(&g_lcSpareWaitId, 0); BlankSpareFireFiles();
        Log("[slice] CreateLine: held callback %llx is neither the line manager's nor the line list's -- spare line %d stays unselected\n", (unsigned long long)(doCall - (uint64_t)g_base), id);
        return;
    }
    // the registry, and the spare's generation in it (alive, or nothing fires)
    SpareResultEntry entry{ id, 0.0, 0 };
    bool alive = false;
    __try {
        const uint64_t reg = owner ? ((uint64_t (*)(uint64_t))((uint64_t)g_base + RVA_REGISTRY_OF))(owner + regOff) : 0;
        if (reg && Readable((void*)(reg + 0xa0), 0x20)) {
            uint64_t sb = 0, se = 0, gens = 0;
            memcpy(&sb, (void*)(reg + 0xa0), 8); memcpy(&se, (void*)(reg + 0xa8), 8); memcpy(&gens, (void*)(reg + 0xb8), 8);
            const uint64_t count = (se > sb) ? (se - sb) / 24 : 0;
            if ((uint64_t)id < count && Readable((void*)(sb + (uint64_t)id * 24), 16) && gens && Readable((void*)(gens + (uint64_t)id * 12), 12)) {
                uint64_t vb = 0, ve = 0;
                memcpy(&vb, (void*)(sb + (uint64_t)id * 24), 8); memcpy(&ve, (void*)(sb + (uint64_t)id * 24 + 8), 8);
                int32_t first = -1;
                if (ve - vb == 8 && Readable((void*)vb, 4)) memcpy(&first, (void*)vb, 4);
                if (ve - vb != 8 || first >= 0) {
                    memcpy(&entry.gen, (void*)(gens + (uint64_t)id * 12), 8);
                    memcpy(&entry.x, (void*)(gens + (uint64_t)id * 12 + 8), 4);
                    alive = true;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { alive = false; }
    if (!alive) {
        if (late) { InterlockedExchange(&g_lcSpareWaitId, 0); BlankSpareFireFiles(); Log("[slice] CreateLine: spare line %d is not alive in the editor's registry -- giving up (the held callback expires)\n", id); }
        return;
    }
    AcquireSRWLockExclusive(&g_lcLock);
    if (!g_lcStash.empty() && g_lcStash.front().fn == buf) g_lcStash.erase(g_lcStash.begin());
    ReleaseSRWLockExclusive(&g_lcLock);
    InterlockedExchange(&g_lcSpareWaitId, 0);
    uint8_t* fakeImpl = (uint8_t*)calloc(1, 0xb20);
    uint64_t* result = (uint64_t*)calloc(8, 8);   // { impl, begin, end, ... }
    bool fired = false;
    if (fakeImpl && result) {
        fakeImpl[0xb18] = 3;
        memcpy(fakeImpl + 0x58, &id, 4);
        result[0] = (uint64_t)fakeImpl;
        result[1] = (uint64_t)&entry;
        result[2] = (uint64_t)&entry + sizeof(entry);
        __try { ((void (*)(uint64_t, uint64_t))doCall)(impl, (uint64_t)result); fired = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { fired = false; }
    }
    free(fakeImpl); free(result);
    if (g_lcSpentFn) free(g_lcSpentFn);
    g_lcSpentFn = buf;
    BlankSpareFireFiles();
    Log(fired ? "[slice] CreateLine: the line editor opened spare line %d (%llu ms after the click)\n"
              : "[slice] CreateLine: firing the held callback on spare line %d faulted -- the editor keeps what it shows\n",
        id, (unsigned long long)(GetTickCount64() - g_lcSpareWaitAt));
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
