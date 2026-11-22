// The Windows save stream (native/src/save_zstd.h over the built-in zstd 1.5.7,
// native/third_party/zstd) in boost's exact call pattern: 64 KiB writes, a flush in
// the middle, then close -- an empty compressStream and endStream until the end
// reports 0 -- round-tripped and timed against one thread. tools\save_zstd_win_test.ps1.
#define ZSTD_MULTITHREAD
#include "../native/third_party/zstd/lib/zstd.h"
#include "../native/src/save_zstd.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

static void* Create() { return ZSTD_createCCtx(); }
static size_t Free(void* c) { return ZSTD_freeCCtx((ZSTD_CCtx*)c); }
static size_t Set(void* c, int p, int v) { return ZSTD_CCtx_setParameter((ZSTD_CCtx*)c, (ZSTD_cParameter)p, v); }
static size_t Reset(void* c, int r) { return ZSTD_CCtx_reset((ZSTD_CCtx*)c, (ZSTD_ResetDirective)r); }
static size_t Stream2(void* c, savezstd::OutBuf* o, savezstd::InBuf* i, int d)
{ return ZSTD_compressStream2((ZSTD_CCtx*)c, (ZSTD_outBuffer*)o, (ZSTD_inBuffer*)i, (ZSTD_EndDirective)d); }
static unsigned IsErr(size_t r) { return ZSTD_isError(r); }
static int gameCalls = 0;
static size_t GInit(void*, int) { return 0; }
static size_t GStream(void*, savezstd::OutBuf*, savezstd::InBuf*) { ++gameCalls; return 0; }
static size_t GOut(void*, savezstd::OutBuf*) { ++gameCalls; return 0; }
static size_t GFree(void*) { return 0; }

static bool Fail(const char* what) { printf("FAIL: %s\n", what); return false; }

static bool RoundTrip(int workers, const std::vector<char>& input, double* ms, size_t* packedSize)
{
    savezstd::Api api;
    api.createCCtx = &Create; api.freeCCtx = &Free; api.setParameter = &Set;
    api.reset = &Reset; api.compressStream2 = &Stream2; api.isError = &IsErr;
    savezstd::Embedded game;
    game.init = &GInit; game.compress = &GStream; game.flush = &GOut; game.end = &GOut; game.free = &GFree;
    savezstd::Streams s(api, game, workers);
    std::vector<char> packed, buf(65536);
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    int cs = 0;
    s.Init(&cs, 1);
    auto take = [&](const savezstd::OutBuf& o) { packed.insert(packed.end(), buf.data(), buf.data() + o.pos); };
    for (size_t at = 0; at < input.size();) {
        savezstd::InBuf in{ input.data() + at, (std::min)(size_t(65536), input.size() - at), 0 };
        while (in.pos < in.size) {
            savezstd::OutBuf o{ buf.data(), buf.size(), 0 };
            if (ZSTD_isError(s.Compress(&cs, &o, &in))) return Fail("compressStream");
            take(o);
        }
        at += in.size;
        if (at == input.size() / 2)
            for (size_t left = 1; left;) {
                savezstd::OutBuf o{ buf.data(), buf.size(), 0 };
                left = s.Flush(&cs, &o);
                if (ZSTD_isError(left)) return Fail("flushStream");
                take(o);
            }
    }
    for (size_t left = 1; left;) {
        savezstd::OutBuf o{ buf.data(), buf.size(), 0 };
        savezstd::InBuf empty{ nullptr, 0, 0 };
        if (ZSTD_isError(s.Compress(&cs, &o, &empty))) return Fail("empty compressStream during the end");
        left = s.End(&cs, &o);
        if (ZSTD_isError(left)) return Fail("endStream");
        take(o);
    }
    s.Free(&cs);
    QueryPerformanceCounter(&t1);
    *ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    *packedSize = packed.size();
    if (gameCalls) return Fail("a call reached the game's compressor");
    if (s.Snapshot().fallbacks) return Fail("no worker context");
    std::vector<char> back(input.size() + 1);
    const size_t n = ZSTD_decompress(back.data(), back.size(), packed.data(), packed.size());
    if (ZSTD_isError(n) || n != input.size() || memcmp(back.data(), input.data(), n)) return Fail("round trip differs");
    return true;
}

int main()
{
    // 256 MiB of varied, compressible bytes (a save compresses ~2.4:1)
    std::vector<char> input(256u << 20);
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < input.size(); ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        input[i] = (x & 3) ? char("saveZSTD"[(x >> 8) & 7]) : char(x >> 16);
    }
    double one = 0, four = 0; size_t p1 = 0, p4 = 0;
    if (!RoundTrip(1, input, &one, &p1) || !RoundTrip(4, input, &four, &p4)) return 1;
    printf("PASS: 256 MiB round trips; 1 worker %.0f ms (%zu bytes), 4 workers %.0f ms (%zu bytes), %.2fx\n",
           one, p1, four, p4, one / four);
    return 0;
}
