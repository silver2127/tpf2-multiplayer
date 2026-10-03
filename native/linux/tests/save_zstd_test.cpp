// savezstd::Streams against a fake libzstd and fake embedded functions, then --
// where the machine has a threaded libzstd >= 1.4 -- a real round trip through it.
#include "save_zstd.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace {
struct FakeCtx { int workers = 0, level = 0, resets = 0, ending = 0; std::vector<int> directives; };
int created = 0, freed = 0;
bool failCreate = false, failWorkers = false;
void* Create() { if (failCreate) return nullptr; ++created; return new FakeCtx; }
size_t FreeCtx(void* c) { ++freed; delete static_cast<FakeCtx*>(c); return 0; }
const size_t kErr = size_t(-1);
unsigned IsError(size_t r) { return r == kErr; }
size_t SetParam(void* c, int p, int v)
{
    auto* x = static_cast<FakeCtx*>(c);
    if (p == savezstd::kWorkers) { if (failWorkers) return kErr; x->workers = v; }
    if (p == savezstd::kLevel) x->level = v;
    return 0;
}
size_t Reset(void* c, int) { auto* x = static_cast<FakeCtx*>(c); ++x->resets; x->ending = 0; return 0; }
// As a multithreaded libzstd: an end or flush takes three calls to drain, and a
// `continue` while one is draining is refused (ZSTDMT: stage_wrong).
size_t Compress2(void* c, savezstd::OutBuf* out, savezstd::InBuf* in, int d)
{
    auto* x = static_cast<FakeCtx*>(c);
    x->directives.push_back(d);
    if (d == savezstd::kContinue && x->ending) return kErr;
    if (d != savezstd::kContinue && in->pos != in->size) return kErr;
    in->pos = in->size;                     // consumes everything
    if (out->pos < out->size) out->pos++;   // one byte out per call
    if (d == savezstd::kContinue) return 7;
    if (!x->ending) x->ending = 3;
    return size_t(--x->ending);             // 2, 1, 0
}

int gInit = 0, gCompress = 0, gFlush = 0, gEnd = 0, gFree = 0, gLevel = 0;
size_t GInit(void*, int level) { ++gInit; gLevel = level; return 0; }
size_t GCompress(void*, savezstd::OutBuf*, savezstd::InBuf*) { ++gCompress; return 11; }
size_t GFlush(void*, savezstd::OutBuf*) { ++gFlush; return 12; }
size_t GEnd(void*, savezstd::OutBuf*) { ++gEnd; return 13; }
size_t GFree(void*) { ++gFree; return 0; }

savezstd::Api FakeApi()
{
    savezstd::Api a;
    a.createCCtx = &Create; a.freeCCtx = &FreeCtx; a.setParameter = &SetParam;
    a.reset = &Reset; a.compressStream2 = &Compress2; a.isError = &IsError;
    return a;
}
savezstd::Embedded FakeGame()
{
    savezstd::Embedded g;
    g.init = &GInit; g.compress = &GCompress; g.flush = &GFlush; g.end = &GEnd; g.free = &GFree;
    return g;
}

void RealRoundTrip()
{
    void* lib = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
    auto version = lib ? reinterpret_cast<unsigned (*)()>(dlsym(lib, "ZSTD_versionNumber")) : nullptr;
    if (!version || version() < 10400) { puts("SKIP real libzstd round trip: none >= 1.4"); return; }
    savezstd::Api a;
    a.createCCtx = reinterpret_cast<void* (*)()>(dlsym(lib, "ZSTD_createCCtx"));
    a.freeCCtx = reinterpret_cast<size_t (*)(void*)>(dlsym(lib, "ZSTD_freeCCtx"));
    a.setParameter = reinterpret_cast<size_t (*)(void*, int, int)>(dlsym(lib, "ZSTD_CCtx_setParameter"));
    a.reset = reinterpret_cast<size_t (*)(void*, int)>(dlsym(lib, "ZSTD_CCtx_reset"));
    a.compressStream2 = reinterpret_cast<size_t (*)(void*, savezstd::OutBuf*, savezstd::InBuf*, int)>(dlsym(lib, "ZSTD_compressStream2"));
    a.isError = reinterpret_cast<unsigned (*)(size_t)>(dlsym(lib, "ZSTD_isError"));
    auto decompress = reinterpret_cast<size_t (*)(void*, size_t, const void*, size_t)>(dlsym(lib, "ZSTD_decompress"));
    assert(a.Complete() && decompress);
    void* probe = a.createCCtx();
    const bool threads = !a.isError(a.setParameter(probe, savezstd::kWorkers, 2));
    a.freeCCtx(probe);
    if (!threads) { puts("SKIP real libzstd round trip: built without threads"); return; }
    // 24 MB of compressible, non-trivial input, fed in boost's 64 KiB steps
    std::vector<char> input(24u << 20);
    uint32_t x = 12345;
    for (size_t i = 0; i < input.size(); ++i) { x = x * 1103515245u + 12345u; input[i] = char("abcdefgh"[(x >> 16) & 7] + (i % 97 == 0)); }
    savezstd::Streams s(a, FakeGame(), 4);
    const int compressBefore = gCompress, endBefore = gEnd;
    int cs = 0;
    s.Init(&cs, 1);
    std::vector<char> packed;
    char buf[65536];
    size_t at = 0;
    while (at < input.size()) {
        savezstd::InBuf in{input.data() + at, std::min<size_t>(65536, input.size() - at), 0};
        while (in.pos < in.size) {
            savezstd::OutBuf out{buf, sizeof(buf), 0};
            assert(!a.isError(s.Compress(&cs, &out, &in)));
            packed.insert(packed.end(), buf, buf + out.pos);
        }
        at += in.size;
    }
    // symmetric_filter::close: deflate(finish) until it reports stream_end, each an
    // empty compressStream and then endStream, the output written after every call
    for (size_t left = 1; left;) {
        savezstd::OutBuf out{buf, sizeof(buf), 0};
        savezstd::InBuf empty{nullptr, 0, 0};
        assert(!a.isError(s.Compress(&cs, &out, &empty)));
        left = s.End(&cs, &out);
        assert(!a.isError(left));
        packed.insert(packed.end(), buf, buf + out.pos);
    }
    s.Free(&cs);
    std::vector<char> back(input.size() + 1);
    const size_t n = decompress(back.data(), back.size(), packed.data(), packed.size());
    assert(!a.isError(n) && n == input.size() && !memcmp(back.data(), input.data(), n));
    assert(gCompress == compressBefore && gEnd == endBefore);   // nothing went to the game's compressor
    printf("PASS real libzstd %u round trip: %zu -> %zu bytes on 4 workers\n", version(), input.size(), packed.size());
}
} // namespace

int main()
{
    {
        savezstd::Streams s(FakeApi(), FakeGame(), 3);
        int a = 0, b = 0, unknown = 0;
        char out[16]; char src[8] = {};
        savezstd::OutBuf o{out, sizeof(out), 0};
        savezstd::InBuf i{src, sizeof(src), 0};
        // a stream nobody initialised is the game's
        assert(s.Compress(&unknown, &o, &i) == 11 && s.Flush(&unknown, &o) == 12 && s.End(&unknown, &o) == 13);
        assert(gCompress == 1 && gFlush == 1 && gEnd == 1);
        // init: the game's stream is initialised too, then ours takes the calls
        assert(s.Init(&a, 1) == 0 && gInit == 1 && gLevel == 1 && created == 1);
        assert(s.Init(&b, 3) == 0 && created == 2);
        i.pos = 0; assert(s.Compress(&a, &o, &i) == 7 && i.pos == 8);
        i.pos = 0; assert(s.Compress(&b, &o, &i) == 7);                 // interleaved: the cache follows
        i.pos = 0; assert(s.Compress(&a, &o, &i) == 7);
        // boost's deflate(finish), repeated until the end reports 0: an empty compressStream,
        // then endStream. The empty call inside the end never reaches the library.
        savezstd::InBuf empty{src, 0, 0};
        size_t left = 1; int rounds = 0;
        while (left) { assert(!IsError(s.Compress(&a, &o, &empty))); left = s.End(&a, &o); assert(!IsError(left)); ++rounds; }
        assert(rounds == 3);
        left = 1; rounds = 0;
        while (left) { assert(!IsError(s.Compress(&b, &o, &empty))); left = s.End(&b, &o); assert(!IsError(left)); ++rounds; }
        assert(rounds == 3);
        // a flush still draining when new input arrives: the flush finishes first, then the input goes in
        int f = 0;
        assert(s.Init(&f, 1) == 0);
        assert(s.Flush(&f, &o) == 2);
        i.pos = 0; assert(s.Compress(&f, &o, &i) == 1 && i.pos == 0);   // drains one step, input waits
        i.pos = 0; assert(s.Compress(&f, &o, &i) == 7 && i.pos == 8);   // drained, input taken
        assert(s.Free(&f) == 0);
        assert(gCompress == 1 && gFlush == 1 && gEnd == 1);             // none reached the game
        // re-init (boost's close resets the stream) reuses the context
        assert(s.Init(&a, 1) == 0 && created == 3);   // a, b and f; a keeps its own
        assert(s.Snapshot().streams == 4 && s.Snapshot().bytesIn == 32 && s.Snapshot().fallbacks == 0);
        // free: ours and the game's; the pointer is the game's again after
        assert(s.Free(&a) == 0 && gFree == 2 && freed == 2);
        i.pos = 0; assert(s.Compress(&a, &o, &i) == 11 && gCompress == 2);
        // a context that cannot get workers is not used
        failWorkers = true;
        int c = 0;
        assert(s.Init(&c, 1) == 0 && freed == 3 && s.Snapshot().fallbacks == 1);
        i.pos = 0; assert(s.Compress(&c, &o, &i) == 11);
        failWorkers = false; failCreate = true;
        int d = 0;
        assert(s.Init(&d, 1) == 0 && s.Snapshot().fallbacks == 2);
        assert(s.End(&d, &o) == 13);
        failCreate = false;
        assert(s.Free(&b) == 0 && freed == 4);
    }
    puts("PASS savezstd stream routing");
    RealRoundTrip();
    return 0;
}
