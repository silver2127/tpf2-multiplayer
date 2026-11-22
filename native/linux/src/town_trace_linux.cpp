// Town development trace, native build 35924 (see town_trace_linux.h and
// native/src/town_trace.h). Diagnostic only; off unless TPF2MP_TOWN_TRACE=1.
#include "town_trace_linux.h"
#include "near_alloc.h"
#include "../../src/town_trace.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
constexpr char kTraceBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
constexpr uintptr_t kDevelopRva = 0x14f5b80;      // TownDeveloper::Develop
constexpr uintptr_t kCallRva = 0x1747917;         // its call in the TownSystem update 0x1746790
constexpr uintptr_t kGuardRva = 0x17478d8;
// The argument setup and the call (this = [ctx+0x40], rsi = engine, edx = town,
// ecx = false, r8 = &mt, r9 = std::optional<float>, pushed IProgressMonitor* 0).
constexpr unsigned char kGuard[] = {
    0x48,0x8b,0x85,0x08,0xf4,0xff,0xff,                    // mov rax,[rbp-0xbf8]
    0x48,0x83,0xec,0x08,                                   // sub rsp,8
    0x31,0xc9,                                             // xor ecx,ecx
    0x44,0x89,0xea,                                        // mov edx,r13d
    0x48,0xbe,0xff,0xff,0xff,0xff,0x00,0xff,0xff,0xff,     // movabs rsi,0xffffff00ffffffff
    0x48,0x21,0xb5,0xc8,0xf3,0xff,0xff,                    // and [rbp-0xc38],rsi
    0x4c,0x8b,0x85,0xa8,0xf3,0xff,0xff,                    // mov r8,[rbp-0xc58]
    0x48,0x8b,0x78,0x40,                                   // mov rdi,[rax+0x40]
    0x48,0x8b,0x85,0xc8,0xf3,0xff,0xff,                    // mov rax,[rbp-0xc38]
    0x6a,0x00,                                             // push 0
    0x48,0x8b,0xb5,0x18,0xf4,0xff,0xff,                    // mov rsi,[rbp-0xbe8]
    0x49,0x89,0xc1,                                        // mov r9,rax
    0xe8,0x64,0xe2,0xda,0xff,                              // call 0x14f5b80
    0x58, 0x5a,                                            // pop rax; pop rdx
};
static_assert(kGuardRva + sizeof(kGuard) == 0x174791e);
static_assert(kCallRva - kGuardRva == 0x3f);

std::atomic<const char*> g_status{"off (not initialized)"};
Tpf2mpTownDevelopFn g_original = nullptr;
std::mutex g_mutex;                     // the file, the engine table, the TF windows
FILE* g_file = nullptr;
unsigned g_unflushed = 0;
TownTraceEngines g_engines;
int64_t g_familyWindow[4] = {-1, -1, -1, -1};
std::atomic<int64_t> g_lastTime{-1};

struct Tick {
    int64_t time = -1;
    uintptr_t begin = 0;
    size_t count = 0;
    uint32_t list = 0;
    bool listOk = false;
};
thread_local Tick t_tick;

void Write(const char* line, int len, bool flush)
{
    if (len <= 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    fwrite(line, 1, size_t(len), g_file);
    if (flush || ++g_unflushed >= 64) { fflush(g_file); g_unflushed = 0; }
}
}

void Tpf2mpTownTraceTick(uint32_t time, uintptr_t context)
{
    if (!tpf2mp_town_trace::on.load(std::memory_order_relaxed)) return;
    Tick tick;
    tick.time = int32_t(time);
    // context+8 -> the Town node vector {begin, end}; nodes are {entity, index}.
    uintptr_t vec = 0, b = 0, e = 0;
    if (context) std::memcpy(&vec, reinterpret_cast<const void*>(context + 8), 8);
    if (vec) { std::memcpy(&b, reinterpret_cast<const void*>(vec), 8); std::memcpy(&e, reinterpret_cast<const void*>(vec + 8), 8); }
    if (b && e >= b && !((e - b) % 8) && (e - b) / 8 < (size_t(1) << 20)) {
        tick.begin = b; tick.count = (e - b) / 8;
        tick.list = TownTraceListDigest(reinterpret_cast<const uint8_t*>(b), tick.count, 8);
        tick.listOk = true;
    }
    t_tick = tick;
    g_lastTime.store(tick.time, std::memory_order_relaxed);
}

void Tpf2mpTownTraceDevelop(void* self, void* engine, uint32_t town, bool flag, void* mt, uint64_t opt, void* progress)
{
    TownTraceDevelop d{};
    d.time = t_tick.time;
    d.town = int32_t(town);
    d.index = -1; d.count = -1;
    if (t_tick.listOk) {
        d.count = int(t_tick.count);
        d.list = t_tick.list;
        d.index = TownTraceIndexOf(reinterpret_cast<const uint8_t*>(t_tick.begin), t_tick.count, 8, int32_t(town));
    }
    d.mt0 = mt ? TownTraceFnv64(mt, TOWN_TRACE_MT_BYTES) : 0;
    g_original(self, engine, town, flag, mt, opt, progress);
    d.mt1 = mt ? TownTraceFnv64(mt, TOWN_TRACE_MT_BYTES) : 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        d.engine = g_engines.IndexOf(reinterpret_cast<uintptr_t>(engine));
    }
    char line[256];
    Write(line, TownTraceFormatTT(line, sizeof(line), d), false);
}

bool Tpf2mpTownTraceFamiliesDue(uintptr_t engine, int64_t* time, int* engineIndex)
{
    if (!tpf2mp_town_trace::on.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    *time = g_lastTime.load(std::memory_order_relaxed);
    *engineIndex = g_engines.IndexOf(engine);
    return TownTraceFamiliesDue(*time, *engineIndex, g_familyWindow, 4);
}

void Tpf2mpTownTraceFamilies(int64_t time, int engineIndex, uint64_t* tokens, size_t count)
{
    char line[2048];
    Write(line, TownTraceFormatTF(line, sizeof(line), time, engineIndex, tokens, count), true);
}

void Tpf2mpTownTraceSetOriginal(Tpf2mpTownDevelopFn original) { g_original = original; }
void Tpf2mpTownTraceFlush() { std::lock_guard<std::mutex> lock(g_mutex); if (g_file) fflush(g_file); }
void Tpf2mpTownTraceOpenForTest(const char* path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = fopen(path, "w");
    g_engines = TownTraceEngines{};
    for (auto& w : g_familyWindow) w = -1;
    tpf2mp_town_trace::on.store(g_file != nullptr);
}

bool Tpf2mpInstallTownTrace(uintptr_t base, const char* buildId, const char* path)
{
    const char* env = std::getenv("TPF2MP_TOWN_TRACE");
    if (!env || std::strcmp(env, "1")) { g_status.store("off (set TPF2MP_TOWN_TRACE=1 to trace town development)"); return false; }
    if (tpf2mp_town_trace::on.load()) { g_status.store("enabled (already installed)"); return false; }
    if (!base || !buildId || std::strcmp(buildId, kTraceBuildId)) { g_status.store("off (unverified image)"); return false; }
    if (std::memcmp(reinterpret_cast<const void*>(base + kGuardRva), kGuard, sizeof(kGuard))) {
        g_status.store("off (unverified TownSystem Develop call)"); return false;
    }
    FILE* f = path ? fopen(path, "a") : nullptr;
    if (!f) { g_status.store("off (trace file not writable)"); return false; }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_file = f;
        fprintf(g_file, "# town trace, native build 35924, format 1\n");
        fflush(g_file);
    }
    g_original = reinterpret_cast<Tpf2mpTownDevelopFn>(base + kDevelopRva);
    tpf2mp_town_trace::on.store(true);
    if (!Tpf2mpRedirectCall(base + kCallRva, base + kDevelopRva, reinterpret_cast<void*>(&Tpf2mpTownTraceDevelop))) {
        tpf2mp_town_trace::on.store(false);
        g_status.store("off (Develop call not redirected; unchanged)");
        return false;
    }
    g_status.store("enabled (TT per Develop call, TF per 600 iterations)");
    return true;
}

const char* Tpf2mpTownTraceStatus() { return g_status.load(); }
