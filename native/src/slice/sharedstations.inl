// sharedstations.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// SHARED STATIONS: the line editor's owner gate in companies mode


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
