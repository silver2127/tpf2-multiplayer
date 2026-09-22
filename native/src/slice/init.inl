// init.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// the relay blobs, hook installation, Init and DllMain


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
    InstallLineAssign();
    // The same independence from the session applies to the road sum and the
    // two claim-order observers ("ROAD FREE SPACE", "SHIP AND AIRCRAFT CLAIM
    // ORDER"): the first bus to reach a junction must get the same float on
    // every peer from the first step.
    InstallRoadSpace();
    InstallRoadEntries();
    // Person batches in entity-id order, so a host that keeps its world at a hot
    // join and the joiner that loads its save decide alike ("HOT-JOIN ORDER").
    InstallHotJoinOrder();
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
