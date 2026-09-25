// town_trace.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// TOWN DEVELOPMENT TRACE (diagnostic, off by default)
//
// `towntrace=1` in tpf2_slice.cfg (read once at attach; restart the game to
// change it) writes tpf2_towntrace.txt in the data dir: one TT line per
// TownDeveloper::Develop call of the TownSystem update, and a TF line of
// node-list digests every 600 TownSystem iterations. The native Linux build
// writes the same lines (TPF2MP_TOWN_TRACE=1); tools/town_trace_diff.py names
// the first one two peers disagree on. Format and meaning: ../town_trace.h.
// Off, nothing is patched and nothing is read. On, it changes no decision:
// the wrapper hands Develop its arguments unchanged and only reads memory.
//
// Site: TownSystem update 0xab1d20, `call TownDeveloper::Develop` at 0xab253e
// (rcx = [r13+0x40], rdx = engine, r8d = town, r9d = 0, [rsp+0x20] = &mt,
// [rsp+0x28] = std::optional<float> {}, [rsp+0x30] = IProgressMonitor* 0).
// r13 is the update's context there: +8 -> the Town node vector {begin, end}
// (nodes {entity, index}, the order the stagger `t % 120 == (i % 30) * 4`
// reads), +0x20 -> the GameTime getter's argument (0x287830 returns
// GameTime+0x34, the same int the update seeds its mt19937 from). The call's
// rel32 is pointed at a near stub that stores r13 and jumps to the wrapper.
#include "../town_trace.h"

static const uintptr_t RVA_TT_SITE = 0xab2518;      // the argument setup ...
static const uintptr_t RVA_TT_CALL = 0xab253e;      // ... and the call
static const uintptr_t RVA_TT_DEVELOP = 0x91d910;   // TownDeveloper::Develop
static const uintptr_t RVA_TT_GETTIME = 0x287830;   // int (void* timeRef): GameTime+0x34
static const uint8_t TT_EXPECT[] = {
    0x33, 0xC0,                                // xor eax, eax
    0x33, 0xC9,                                // xor ecx, ecx
    0x48, 0x89, 0x4C, 0x24, 0x30,              // mov [rsp+0x30], rcx   (IProgressMonitor* = 0)
    0x48, 0x89, 0x44, 0x24, 0x28,              // mov [rsp+0x28], rax   (optional<float> = {})
    0x48, 0x8D, 0x45, 0x40,                    // lea rax, [rbp+0x40]   (the tick's mt19937)
    0x48, 0x89, 0x44, 0x24, 0x20,              // mov [rsp+0x20], rax
    0x45, 0x33, 0xC9,                          // xor r9d, r9d          (bool = false)
    0x44, 0x8B, 0x44, 0x24, 0x40,              // mov r8d, [rsp+0x40]   (the town)
    0x49, 0x8B, 0xD4,                          // mov rdx, r12          (the engine)
    0x49, 0x8B, 0x4D, 0x40,                    // mov rcx, [r13+0x40]   (TownDeveloper)
    0xE8, 0xCD, 0xB3, 0xE6, 0xFF               // call 0x91d910          (0xab253e)
};
static const uint8_t TT_GETTIME_EXPECT[] = {   // 0x287830, 25 bytes: a pure getter
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x51, 0x10, 0x48, 0x8B, 0x49, 0x08,
    0xE8, 0x0F, 0x9A, 0xFF, 0xFF, 0x8B, 0x40, 0x34, 0x48, 0x83, 0xC4, 0x28, 0xC3
};
static_assert(sizeof(TT_EXPECT) == RVA_TT_CALL + 5 - RVA_TT_SITE, "guard ends with the call");

typedef void (*TtDevelopFn)(void*, void*, uint32_t, bool, void*, uint64_t, void*);
typedef int (*TtGetTimeFn)(void*);
static bool g_ttOn = false;
static FILE* g_ttFile = nullptr;
static SRWLOCK g_ttLock = SRWLOCK_INIT;
static TownTraceEngines g_ttEngines;
static int64_t g_ttWindow[4] = { -1, -1, -1, -1 };
static volatile LONG64 g_ttLastTime = -1;
static unsigned g_ttUnflushed = 0;
static volatile uintptr_t* g_ttCtxSlot = nullptr;    // in the near stub: r13 at the call
static TtDevelopFn g_ttOriginal = nullptr;
static volatile LONG64 g_ttCalls = 0, g_ttFaults = 0;

static void TownTraceWrite(const char* line, int len, bool flush)
{
    if (len <= 0) return;
    AcquireSRWLockExclusive(&g_ttLock);
    if (g_ttFile) {
        fwrite(line, 1, (size_t)len, g_ttFile);
        if (flush || ++g_ttUnflushed >= 64) { fflush(g_ttFile); g_ttUnflushed = 0; }
    }
    ReleaseSRWLockExclusive(&g_ttLock);
}

// The tick's clock and Town list, read defensively (no C++ objects here, so
// __try is allowed). The list and the time are what the update itself read.
struct TtTick { int64_t time; uintptr_t begin; size_t count; uint32_t list; bool listOk; };
static void TownTraceReadTick(uintptr_t ctx, TtTick* t)
{
    t->time = -1; t->begin = 0; t->count = 0; t->list = 0; t->listOk = false;
    if (!ctx) return;
    __try {
        void* timeRef = *(void**)(ctx + 0x20);
        if (timeRef) t->time = ((TtGetTimeFn)(g_base + RVA_TT_GETTIME))(timeRef);
        const uintptr_t vec = *(uintptr_t*)(ctx + 8);
        if (vec && Readable((void*)vec, 16)) {
            const uintptr_t b = *(uintptr_t*)vec, e = *(uintptr_t*)(vec + 8);
            if (b && e >= b && !((e - b) % 8) && (e - b) / 8 < ((size_t)1 << 20) && Readable((void*)b, (size_t)(e - b))) {
                t->begin = b; t->count = (size_t)(e - b) / 8;
                t->list = TownTraceListDigest((const uint8_t*)b, t->count, 8);
                t->listOk = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(&g_ttFaults);
        t->listOk = false;
    }
}

static void TownTraceDevelopWrap(void* self, void* engine, uint32_t town, bool flag, void* mt, uint64_t opt, void* progress)
{
    const uintptr_t ctx = g_ttCtxSlot ? *g_ttCtxSlot : 0;
    static uintptr_t lastCtx = 0;             // the sim thread only: one tick's list read once
    static TtTick tick = { -1, 0, 0, 0, false };
    TtTick now;
    TownTraceReadTick(ctx, &now);
    if (now.time != tick.time || ctx != lastCtx || !tick.listOk) { tick = now; lastCtx = ctx; }
    InterlockedExchange64(&g_ttLastTime, tick.time);
    TownTraceDevelop d = {};
    d.time = tick.time;
    d.town = (int32_t)town;
    d.index = -1; d.count = -1;
    if (tick.listOk) {
        d.count = (int)tick.count;
        d.list = tick.list;
        d.index = TownTraceIndexOf((const uint8_t*)tick.begin, tick.count, 8, (int32_t)town);
    }
    d.mt0 = mt ? TownTraceFnv64(mt, TOWN_TRACE_MT_BYTES) : 0;
    g_ttOriginal(self, engine, town, flag, mt, opt, progress);
    d.mt1 = mt ? TownTraceFnv64(mt, TOWN_TRACE_MT_BYTES) : 0;
    AcquireSRWLockExclusive(&g_ttLock);
    d.engine = g_ttEngines.IndexOf((uintptr_t)engine);
    ReleaseSRWLockExclusive(&g_ttLock);
    InterlockedIncrement64(&g_ttCalls);
    char line[256];
    TownTraceWrite(line, TownTraceFormatTT(line, sizeof(line), d), false);
}

// Called by the per-iteration family sort (hotjoin_order.inl, site "step").
static bool TownTraceFamiliesWanted(uint8_t* engine, int64_t* time, int* engineIndex)
{
    if (!g_ttOn) return false;
    AcquireSRWLockExclusive(&g_ttLock);
    *time = g_ttLastTime;
    *engineIndex = g_ttEngines.IndexOf((uintptr_t)engine);
    const bool due = TownTraceFamiliesDue(*time, *engineIndex, g_ttWindow, 4);
    ReleaseSRWLockExclusive(&g_ttLock);
    return due;
}
static void TownTraceFamilies(int64_t time, int engineIndex, uint64_t* tokens, size_t n)
{
    char line[2048];
    TownTraceWrite(line, TownTraceFormatTF(line, sizeof(line), time, engineIndex, tokens, n), true);
}

static void InstallTownTrace()
{
    if (!CfgFlag("towntrace", false)) {
        Log("[towntrace] off (towntrace=1 in tpf2_slice.cfg traces town development to tpf2_towntrace.txt)\n");
        return;
    }
    if (!Readable((void*)(g_base + RVA_TT_SITE), sizeof(TT_EXPECT)) ||
        memcmp((void*)(g_base + RVA_TT_SITE), TT_EXPECT, sizeof(TT_EXPECT)) != 0 ||
        memcmp((void*)(g_base + RVA_TT_GETTIME), TT_GETTIME_EXPECT, sizeof(TT_GETTIME_EXPECT)) != 0) {
        Log("[towntrace] NOT installed: the TownSystem's Develop call (rva=%llx) or the GameTime getter is not "
            "build 35924's -- nothing patched\n", (unsigned long long)RVA_TT_CALL);
        return;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%stpf2_towntrace.txt", g_dataDir);
    FILE* f = _fsopen(path, "ab", _SH_DENYWR);
    if (!f) { Log("[towntrace] NOT installed: cannot open %s\n", path); return; }
    // near stub: mov [rip+0x0e], r13 ; jmp [rip+0] <wrapper> ; <slot>
    uint8_t* stub = NearAlloc(32);
    if (!stub) { fclose(f); Log("[towntrace] NOT installed: no near page\n"); return; }
    const int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(g_base + RVA_TT_CALL + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) { fclose(f); Log("[towntrace] NOT installed: stub out of reach\n"); return; }
    static const uint8_t head[] = { 0x4C, 0x89, 0x2D, 0x0E, 0x00, 0x00, 0x00, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    memcpy(stub, head, sizeof(head));
    const uintptr_t wrap = (uintptr_t)&TownTraceDevelopWrap;
    memcpy(stub + 13, &wrap, 8);
    memset(stub + 21, 0, 8);
    g_ttCtxSlot = (volatile uintptr_t*)(stub + 21);
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    g_ttOriginal = (TtDevelopFn)(g_base + RVA_TT_DEVELOP);
    g_ttFile = f;
    fprintf(g_ttFile, "# town trace, Windows build 35924, format 1\n");
    fflush(g_ttFile);
    g_ttOn = true;
    DWORD old = 0;
    const int32_t r32 = (int32_t)rel;
    if (!VirtualProtect((void*)(g_base + RVA_TT_CALL + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
        g_ttOn = false;
        Log("[towntrace] NOT installed: VirtualProtect failed -- nothing patched\n");
        return;
    }
    memcpy((void*)(g_base + RVA_TT_CALL + 1), &r32, 4);
    VirtualProtect((void*)(g_base + RVA_TT_CALL + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(g_base + RVA_TT_CALL), 5);
    Log("[towntrace] installed: TownSystem Develop call rva=%llx -> near stub %p -> wrapper; writing %s "
        "(TT per Develop call, TF per 600 iterations)\n", (unsigned long long)RVA_TT_CALL, (void*)stub, path);
}
