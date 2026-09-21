// ui_tints.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// paused tick, icons for every player, company-colour tints on icons, labels and windows


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
