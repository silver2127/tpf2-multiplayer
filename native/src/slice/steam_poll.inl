// steam_poll.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// THE GAME'S STEAM POLL THREAD, THROTTLED WHILE BUSY (build 35924)
//
// The game runs one thread (loop at 0x26aad70) that polls Steam: 0x26a9160 takes a
// Steam interface and makes several calls on it, then SteamAPI_RunCallbacks. When
// the byte at +0x130 of its object is clear the loop sleeps 200 ms between polls;
// while it is set -- through a world load -- the loop has no sleep at all, and each
// pass is IPC to the Steam client. Measured on the Linux build of the same loop
// (0x36b1960): ~60% of a core for the whole load (2026-09-27; linux/src/slice/
// steam_poll_linux.cpp). The Windows loop is the same code.
//
// The loop's call to 0x26a9160 now reaches SteamPollThrottled, which lets it run at
// most once per steam_poll_ms (default 2; Sleep's granularity may stretch that):
// every poll and every RunCallbacks still happens, in the same order, on the same
// thread. The 200 ms idle path is never slowed. steam_poll_ms=0 turns it off.
static const uintptr_t RVA_STEAMPOLL_LOOP = 0x26aad70, RVA_STEAMPOLL_SITE = 0x26aade7, RVA_STEAMPOLL_FN = 0x26a9160;
static const uint8_t STEAMPOLL_EXPECT[135] = {
    0x48,0x8b,0x03,0x48,0x8b,0x40,0x08,0x0f,0xb6,0x88,0x00,0x02,0x00,0x00,0x84,0xc9,0x75,0x75,0x48,0x8b,0x0b,0x48,0x8b,0x41,
    0x08,0x80,0xb8,0x30,0x01,0x00,0x00,0x00,0x75,0x51,0xe8,0x89,0xb5,0x54,0x00,0xe8,0x1e,0xb5,0x54,0x00,0x48,0x05,0x80,0x84,
    0x1e,0x00,0x48,0x6b,0xc8,0x64,0x48,0x8b,0xc7,0x48,0xf7,0xe9,0x48,0xc1,0xfa,0x1a,0x48,0x8b,0xc2,0x48,0xc1,0xe8,0x3f,0x48,
    0x03,0xd0,0x48,0x89,0x54,0x24,0x20,0x69,0xc2,0x00,0xca,0x9a,0x3b,0x2b,0xc8,0x89,0x4c,0x24,0x28,0x0f,0x28,0x44,0x24,0x20,
    0x66,0x0f,0x7f,0x44,0x24,0x40,0x48,0x8d,0x4c,0x24,0x40,0xe8,0x58,0xb5,0x54,0x00,0x48,0x8b,0x0b,0x48,0x8b,0x49,0x08,0xe8,
    0x74,0xe3,0xff,0xff,0xff,0x15,0xce,0x13,0x86,0x00,0xe9,0x79,0xff,0xff,0xff };
typedef void (*SteamPollFn)(void* self);
static SteamPollFn g_steamPoll = nullptr;
static LONGLONG g_steamPollTicks = 0;   // the minimum interval, in QPC ticks

static void SteamPollThrottled(void* self)
{
    static thread_local LONGLONG last = 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    while (last && now.QuadPart - last < g_steamPollTicks) {
        Sleep(1);
        QueryPerformanceCounter(&now);
    }
    last = now.QuadPart;
    g_steamPoll(self);
}

static int SteamPollFlagMs()
{
    for (int i = 0; i < 2; i++) {
        const char* dir = i == 0 ? g_dllDir : g_dataDir;
        if (!dir[0]) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", dir);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (!f) continue;
        char line[256]; int n = -1;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "steam_poll_ms=", 14)) n = atoi(line + 14);
        fclose(f);
        return n;
    }
    return -1;
}

static void InstallSteamPoll()
{
    int ms = SteamPollFlagMs();
    if (ms == 0) { Log("[steampoll] OFF (steam_poll_ms=0) -- the game's Steam poll spins while it is busy\n"); return; }
    if (ms < 0) ms = 2;
    if (ms > 100) ms = 100;
    if (!BytesAre(RVA_STEAMPOLL_LOOP, STEAMPOLL_EXPECT, sizeof(STEAMPOLL_EXPECT), "steampoll")) return;
    const uintptr_t at = g_base + RVA_STEAMPOLL_SITE;
    int32_t rel = 0; memcpy(&rel, (const void*)(at + 1), 4);
    if (*(const uint8_t*)at != 0xE8 || at + 5 + (int64_t)rel != g_base + RVA_STEAMPOLL_FN) {
        Log("[steampoll] NOT installed: the call at rva=%llx is not the poll\n", (unsigned long long)RVA_STEAMPOLL_SITE);
        return;
    }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_steamPollTicks = f.QuadPart * ms / 1000;
    g_steamPoll = (SteamPollFn)(g_base + RVA_STEAMPOLL_FN);
    uint8_t* stub = NearAlloc(16);
    if (!stub) { Log("[steampoll] NOT installed: no page for the stub\n"); return; }
    const uintptr_t target = (uintptr_t)&SteamPollThrottled;
    stub[0] = 0x48; stub[1] = 0xB8; memcpy(stub + 2, &target, 8);   // mov rax, SteamPollThrottled
    stub[10] = 0xFF; stub[11] = 0xE0;                                  // jmp rax (the call's return address stays)
    FlushInstructionCache(GetCurrentProcess(), stub, 12);
    const int64_t nrel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) { Log("[steampoll] NOT installed: stub out of reach\n"); return; }
    DWORD old = 0;
    if (!VirtualProtect((void*)(at + 1), 4, PAGE_EXECUTE_READWRITE, &old)) { Log("[steampoll] NOT installed: could not unprotect\n"); return; }
    const int32_t r32 = (int32_t)nrel;
    memcpy((void*)(at + 1), &r32, 4);
    VirtualProtect((void*)(at + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    Log("[steampoll] installed: the game's Steam poll runs at most every %d ms while busy (was a spin; idle stays 200 ms)\n", ms);
}
