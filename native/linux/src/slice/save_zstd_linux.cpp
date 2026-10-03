// save_zstd_linux.cpp -- see save_zstd_linux.h. Build 35924 only; the slice has
// already refused any other build before this runs.
#include "save_zstd_linux.h"
#include "slice_core.h"
#include "near_alloc.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace {
// boost::iostreams::detail::zstd_base, statically linked (boost 1.76). Every
// guarded function is whole; the call sites below are inside them.
struct Guard { uintptr_t rva; size_t size; const char* bytes; };
const Guard kGuards[] = {
    {0x3563210, 67,   // ~zstd_base: ZSTD_freeCStream(cstream_) at 3563223
        "\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x53\x48\x89\xfb\x48\x83\xec\x08\x48\x8b\x3f\xe8\xf8\x59\x82\x00"
        "\x48\x8b\x7b\x08\xe8\xaf\x95\x88\x00\x48\x8b\x7b\x10\xbe\x18\x00\x00\x00\xe8\xa1\x8c\x17\xfd\x48"
        "\x8b\x7b\x18\x48\x83\xc4\x08\xbe\x18\x00\x00\x00\x5b\x5d\xe9\x8d\x8c\x17\xfd"},
    {0x3563340, 162,  // deflate(action): compressStream 3563379, flushStream 3563399 (action 1), endStream 35633d8 (action 0)
        "\xf3\x0f\x1e\xfa\x8b\x47\x20\x48\x8b\x57\x10\x85\xc0\x74\x0c\x48\x83\x7a\x08\x00\xb8\x01\x00\x00"
        "\x00\x74\x75\x55\x48\x89\xe5\x41\x56\x41\x55\x41\x54\x41\x89\xf4\x53\x4c\x8b\x77\x18\x48\x89\xfb"
        "\x4c\x8b\x2f\x4c\x89\xf6\x4c\x89\xef\xe8\xc2\xa5\x82\x00\x48\x89\xc7\xe8\x4a\xff\xff\xff\x31\xc0"
        "\x41\x83\xfc\x02\x74\x31\x4c\x89\xf6\x4c\x89\xef\x45\x85\xe4\x74\x3f\xe8\x32\xa8\x82\x00\x49\x89"
        "\xc5\x4c\x89\xef\xe8\x27\xff\xff\xff\x4d\x85\xed\x0f\x94\xc0\x31\xd2\x45\x85\xe4\x0f\x94\xc2\x21"
        "\xc2\x0f\xb6\xc0\x89\x53\x20\x5b\x41\x5c\x41\x5d\x41\x5e\x5d\xc3\x0f\x1f\x84\x00\x00\x00\x00\x00"
        "\xc3\x0f\x1f\x80\x00\x00\x00\x00\xe8\x43\xa8\x82\x00\x49\x89\xc5\xeb\xbf"},
    {0x3563470, 108,  // reset(compress, realloc): initCStream(cstream_, level) at 35634ce
        "\xf3\x0f\x1e\xfa\x84\xd2\x75\x08\xc3\x0f\x1f\x80\x00\x00\x00\x00\x55\x48\x8b\x47\x18\x66\x0f\xef"
        "\xc0\x48\x8b\x57\x10\x48\x89\xe5\x0f\x11\x02\x48\xc7\x42\x10\x00\x00\x00\x00\x0f\x11\x00\x48\xc7"
        "\x40\x10\x00\x00\x00\x00\xc7\x47\x20\x00\x00\x00\x00\x40\x84\xf6\x75\x16\x48\x8b\x7f\x08\xe8\xf5"
        "\x9c\x88\x00\x5d\x48\x89\xc7\xe9\x0c\xfe\xff\xff\x0f\x1f\x40\x00\x8b\x77\x24\x48\x8b\x3f\xe8\xdd"
        "\x77\x82\x00\x5d\x48\x89\xc7\xe9\xf4\xfd\xff\xff"},
    {0x35634e0, 98,   // do_init(params, compress): initCStream(cstream_, params.level) at 356351d
        "\xf3\x0f\x1e\xfa\x55\x48\x8b\x47\x18\x66\x0f\xef\xc0\x48\x8b\x4f\x10\x0f\x11\x01\x48\x89\xe5\x48"
        "\xc7\x41\x10\x00\x00\x00\x00\x0f\x11\x00\x48\xc7\x40\x10\x00\x00\x00\x00\x8b\x36\xc7\x47\x20\x00"
        "\x00\x00\x00\x89\x77\x24\x84\xd2\x74\x16\x48\x8b\x3f\xe8\x8e\x77\x82\x00\x5d\x48\x89\xc7\xe9\xa5"
        "\xfd\xff\xff\x0f\x1f\x44\x00\x00\x48\x8b\x7f\x08\xe8\x77\x9c\x88\x00\x5d\x48\x89\xc7\xe9\x8e\xfd"
        "\xff\xff"},
};
// The embedded zstd 1.5.2 functions the sites call.
constexpr uintptr_t kInitCStream = 0x3d8acb0, kCompressStream = 0x3d8d940, kFlushStream = 0x3d8dbd0,
                    kEndStream = 0x3d8dc20, kFreeCStream = 0x3d88c20;

savezstd::Streams* g_streams = nullptr;
std::atomic<bool> g_takeover{false};
size_t (*g_gameInit)(void*, int) = nullptr;

size_t HookInit(void* cs, int level)
{
    return g_takeover.load(std::memory_order_acquire) ? g_streams->Init(cs, level) : g_gameInit(cs, level);
}
size_t HookCompress(void* cs, savezstd::OutBuf* o, savezstd::InBuf* i) { return g_streams->Compress(cs, o, i); }
size_t HookFlush(void* cs, savezstd::OutBuf* o) { return g_streams->Flush(cs, o); }
size_t HookEnd(void* cs, savezstd::OutBuf* o) { return g_streams->End(cs, o); }
size_t HookFree(void* cs) { return g_streams->Free(cs); }

// save_threads=N from tpf2_menu_flags.txt (the game folder's file first, as the
// other flags); -1 when absent.
int FlagThreads(const char* root, const char* data)
{
    for (const char* dir : {root, data}) {
        if (!dir || !*dir) continue;
        FILE* f = fopen((std::string(dir) + "/tpf2_menu_flags.txt").c_str(), "r");
        if (!f) continue;
        char line[256]; int n = -1;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "save_threads=", 13)) n = atoi(line + 13);
        fclose(f);
        return n;
    }
    return -1;
}

bool LoadApi(savezstd::Api* api, char* why, size_t cap)
{
    void* lib = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { snprintf(why, cap, "no libzstd.so.1 (%s)", dlerror()); return false; }
    auto version = reinterpret_cast<unsigned (*)()>(dlsym(lib, "ZSTD_versionNumber"));
    const unsigned v = version ? version() : 0;
    if (v < 10400) { snprintf(why, cap, "libzstd %u is older than 1.4.0", v); return false; }
    api->createCCtx = reinterpret_cast<void* (*)()>(dlsym(lib, "ZSTD_createCCtx"));
    api->freeCCtx = reinterpret_cast<size_t (*)(void*)>(dlsym(lib, "ZSTD_freeCCtx"));
    api->setParameter = reinterpret_cast<size_t (*)(void*, int, int)>(dlsym(lib, "ZSTD_CCtx_setParameter"));
    api->reset = reinterpret_cast<size_t (*)(void*, int)>(dlsym(lib, "ZSTD_CCtx_reset"));
    api->compressStream2 = reinterpret_cast<size_t (*)(void*, savezstd::OutBuf*, savezstd::InBuf*, int)>(
        dlsym(lib, "ZSTD_compressStream2"));
    api->isError = reinterpret_cast<unsigned (*)(size_t)>(dlsym(lib, "ZSTD_isError"));
    if (!api->Complete()) { snprintf(why, cap, "libzstd %u lacks the streaming API", v); return false; }
    // A libzstd built without threads refuses nbWorkers > 0.
    void* probe = api->createCCtx();
    const bool threads = probe && !api->isError(api->setParameter(probe, savezstd::kWorkers, 2));
    if (probe) api->freeCCtx(probe);
    if (!threads) { snprintf(why, cap, "libzstd %u was built without threads", v); return false; }
    snprintf(why, cap, "libzstd %u.%u.%u", v / 10000, v / 100 % 100, v % 100);
    return true;   // the library stays loaded for the life of the process
}
} // namespace

bool SliceInstallSaveZstd(uintptr_t base, const char* root, const char* data)
{
    int workers = FlagThreads(root, data);
    if (workers == 0) {
        SliceLog("[savezstd] OFF (save_threads=0 in tpf2_menu_flags.txt) -- saves compress on the saving thread\n");
        return false;
    }
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (workers < 0) workers = 4;
    if (workers > 16) workers = 16;
    if (cpus > 1 && workers > cpus - 1) workers = int(cpus - 1);
    if (workers < 1) workers = 1;
    for (const auto& g : kGuards) {
        char bytes[256];
        if (!SliceRead(base + g.rva, bytes, g.size) || memcmp(bytes, g.bytes, g.size)) {
            SliceLog("[savezstd] byte guard failed at %lx -- saves compress as the game does\n", (unsigned long)g.rva);
            return false;
        }
    }
    savezstd::Api api;
    char why[160];
    if (!LoadApi(&api, why, sizeof(why))) {
        SliceLog("[savezstd] OFF: %s -- saves compress on the saving thread\n", why);
        return false;
    }
    savezstd::Embedded game;
    game.init = reinterpret_cast<size_t (*)(void*, int)>(base + kInitCStream);
    g_gameInit = game.init;
    game.compress = reinterpret_cast<size_t (*)(void*, savezstd::OutBuf*, savezstd::InBuf*)>(base + kCompressStream);
    game.flush = reinterpret_cast<size_t (*)(void*, savezstd::OutBuf*)>(base + kFlushStream);
    game.end = reinterpret_cast<size_t (*)(void*, savezstd::OutBuf*)>(base + kEndStream);
    game.free = reinterpret_cast<size_t (*)(void*)>(base + kFreeCStream);
    g_streams = new (std::nothrow) savezstd::Streams(api, game, workers);   // lives as long as the process
    if (!g_streams) return false;
    // Each redirect is independent and keeps the call's own target check. The
    // free goes first and the inits last: a stream is only ever taken over once
    // every call that may reach it already comes here.
    struct Site { uintptr_t at, callee; void* to; };
    const Site sites[] = {
        {0x3563223, kFreeCStream, (void*)&HookFree},
        {0x3563379, kCompressStream, (void*)&HookCompress},
        {0x3563399, kFlushStream, (void*)&HookFlush},
        {0x35633d8, kEndStream, (void*)&HookEnd},
        {0x35634ce, kInitCStream, (void*)&HookInit},
        {0x356351d, kInitCStream, (void*)&HookInit},
    };
    int done = 0;
    for (const auto& s : sites) {
        if (!Tpf2mpRedirectCall(base + s.at, base + s.callee, s.to)) break;
        ++done;
    }
    if (done != 6) {
        // Init delegates to the embedded API until all six redirects succeed;
        // takeover stays disabled even when the first init redirect succeeded.
        SliceLog("[savezstd] only %d of 6 call sites redirected -- no stream is taken over\n", done);
        return false;
    }
    g_takeover.store(true, std::memory_order_release);
    SliceLog("[savezstd] installed: save compression runs on %d libzstd worker threads (%s; save_threads=0 turns it off)\n",
             workers, why);
    return true;
}

void SliceSaveZstdLogAlive()
{
    if (!g_streams) return;
    static uint64_t lastStreams = ~0ull;
    const auto s = g_streams->Snapshot();
    if (s.streams == lastStreams) return;
    lastStreams = s.streams;
    SliceLog("[savezstd] streams=%llu fallbacks=%llu input=%.1f MiB\n", (unsigned long long)s.streams,
             (unsigned long long)s.fallbacks, double(s.bytesIn) / 1048576.0);
}
