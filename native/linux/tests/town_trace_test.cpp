// Town development trace: the shared formatting, the Develop wrapper, the
// family lines, and the installer's refusals, in a private fake image.
#include "../src/town_trace_linux.h"
#include "../../src/town_trace.h"
#include "../src/codewrite_linux.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

static std::string ReadAll(const char* path)
{
    std::string s;
    if (FILE* f = fopen(path, "rb")) {
        char b[4096]; size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
        fclose(f);
    }
    return s;
}

static void HeaderTests()
{
    // FNV-1a reference values.
    assert(TownTraceFnv32("", 0) == 0x811c9dc5u);
    assert(TownTraceFnv32("a", 1) == 0xe40c292cu);
    assert(TownTraceFnv64("a", 1) == 0xaf63dc4c8601ec8cull);
    // List digest covers the entity only, in order.
    const int32_t a[] = {5, 100, 7, 200, 9, 300};
    const int32_t b[] = {5, 111, 7, 222, 9, 333};
    const int32_t c[] = {7, 200, 5, 100, 9, 300};
    const auto* pa = reinterpret_cast<const uint8_t*>(a);
    assert(TownTraceListDigest(pa, 3, 8) == TownTraceListDigest(reinterpret_cast<const uint8_t*>(b), 3, 8));
    assert(TownTraceListDigest(pa, 3, 8) != TownTraceListDigest(reinterpret_cast<const uint8_t*>(c), 3, 8));
    assert(TownTraceIndexOf(pa, 3, 8, 7) == 1 && TownTraceIndexOf(pa, 3, 8, 9) == 2 && TownTraceIndexOf(pa, 3, 8, 8) == -1);
    // Engines by first appearance.
    TownTraceEngines e;
    assert(e.IndexOf(0x1000) == 0 && e.IndexOf(0x2000) == 1 && e.IndexOf(0x1000) == 0);
    // TT format is fixed: tools/town_trace_diff.py parses it.
    TownTraceDevelop d{65515, 4242, 17, 422, 0x1234abcdu, 0x1111222233334444ull, 0x5555666677778888ull, 1};
    char line[256];
    TownTraceFormatTT(line, sizeof(line), d);
    assert(!strcmp(line, "TT t=65515 town=4242 i=17 n=422 list=1234abcd mt0=1111222233334444 mt1=5555666677778888 e=1\n"));
    // TF tokens are sorted, so family map order does not matter.
    uint64_t t1[] = {(3ull << 32) | 0xbu, (1ull << 32) | 0xau, (3ull << 32) | 0x1u};
    uint64_t t2[] = {(3ull << 32) | 0x1u, (3ull << 32) | 0xbu, (1ull << 32) | 0xau};
    char l1[256], l2[256];
    TownTraceFormatTF(l1, sizeof(l1), 1200, 0, t1, 3);
    TownTraceFormatTF(l2, sizeof(l2), 1200, 0, t2, 3);
    assert(!strcmp(l1, l2) && !strcmp(l1, "TF t=1200 e=0 lists=3 1:0000000a 3:00000001 3:0000000b\n"));
    // Cadence: once per engine per 600-iteration window.
    int64_t last[4] = {-1, -1, -1, -1};
    assert(TownTraceFamiliesDue(600, 0, last, 4) && !TownTraceFamiliesDue(1199, 0, last, 4));
    assert(TownTraceFamiliesDue(1199, 1, last, 4) && TownTraceFamiliesDue(1200, 0, last, 4));
    assert(!TownTraceFamiliesDue(-1, 0, last, 4) && !TownTraceFamiliesDue(5000, 7, last, 4));
}

// A stand-in for TownDeveloper::Develop: records its arguments, advances the
// "generator" by one word.
static struct { void *self, *engine, *mt, *progress; uint32_t town; bool flag; uint64_t opt; int calls; } g_seen;
static void FakeDevelop(void* self, void* engine, uint32_t town, bool flag, void* mt, uint64_t opt, void* progress)
{
    g_seen = {self, engine, mt, progress, town, flag, opt, g_seen.calls + 1};
    reinterpret_cast<uint32_t*>(mt)[5] ^= 0xdeadbeef;
}

static void WrapperTests()
{
    char path[] = "/tmp/town_trace_testXXXXXX";
    const int fd = mkstemp(path); assert(fd >= 0); close(fd);
    Tpf2mpTownTraceOpenForTest(path);
    Tpf2mpTownTraceSetOriginal(FakeDevelop);
    // The TownSystem context: +8 -> {begin, end} of {entity, index} nodes.
    int32_t nodes[] = {11, 0, 22, 1, 33, 2, 44, 3};
    uintptr_t vec[3] = {reinterpret_cast<uintptr_t>(nodes), reinterpret_cast<uintptr_t>(nodes + 8), reinterpret_cast<uintptr_t>(nodes + 8)};
    uintptr_t ctx[8] = {0, reinterpret_cast<uintptr_t>(vec)};
    Tpf2mpTownTraceTick(1234, reinterpret_cast<uintptr_t>(ctx));
    std::vector<uint8_t> mt(TOWN_TRACE_MT_BYTES, 7);
    const uint64_t before = TownTraceFnv64(mt.data(), mt.size());
    int self, engine, progress;
    Tpf2mpTownTraceDevelop(&self, &engine, 33, true, mt.data(), 0x0000000100000000ull, &progress);
    const uint64_t after = TownTraceFnv64(mt.data(), mt.size());
    // Arguments reach the original unchanged, exactly once.
    assert(g_seen.calls == 1 && g_seen.self == &self && g_seen.engine == &engine && g_seen.town == 33 &&
           g_seen.flag && g_seen.mt == mt.data() && g_seen.opt == 0x0000000100000000ull && g_seen.progress == &progress);
    // A different engine, a town not in the list, then the family line.
    int engine2;
    Tpf2mpTownTraceDevelop(&self, &engine2, 55, false, mt.data(), 0, nullptr);
    int64_t time = 0; int index = -1;
    assert(Tpf2mpTownTraceFamiliesDue(reinterpret_cast<uintptr_t>(&engine), &time, &index) && time == 1234 && index == 0);
    assert(!Tpf2mpTownTraceFamiliesDue(reinterpret_cast<uintptr_t>(&engine), &time, &index));
    uint64_t tokens[] = {(4ull << 32) | 0x77u, (1ull << 32) | 0x66u};
    Tpf2mpTownTraceFamilies(time, index, tokens, 2);
    Tpf2mpTownTraceFlush();
    const uint32_t list = TownTraceListDigest(reinterpret_cast<const uint8_t*>(nodes), 4, 8);
    char expect[1024];
    snprintf(expect, sizeof(expect),
             "TT t=1234 town=33 i=2 n=4 list=%08x mt0=%016llx mt1=%016llx e=0\n"
             "TT t=1234 town=55 i=-1 n=4 list=%08x mt0=%016llx mt1=%016llx e=1\n"
             "TF t=1234 e=0 lists=2 1:00000066 4:00000077\n",
             list, (unsigned long long)before, (unsigned long long)after,
             list, (unsigned long long)after, (unsigned long long)TownTraceFnv64(mt.data(), mt.size()));
    const std::string got = ReadAll(path);
    if (got != expect) { fprintf(stderr, "got:\n%s\nexpected:\n%s\n", got.c_str(), expect); assert(false); }
    // A context the trace cannot read as a node vector: still one line, index unknown.
    uintptr_t badVec[3] = {reinterpret_cast<uintptr_t>(nodes) + 4, reinterpret_cast<uintptr_t>(nodes), 0};
    ctx[1] = reinterpret_cast<uintptr_t>(badVec);
    Tpf2mpTownTraceTick(1238, reinterpret_cast<uintptr_t>(ctx));
    Tpf2mpTownTraceDevelop(&self, &engine, 33, false, mt.data(), 0, nullptr);
    Tpf2mpTownTraceFlush();
    assert(ReadAll(path).find("TT t=1238 town=33 i=-1 n=-1 list=00000000") != std::string::npos);
    unlink(path);
}

static void InstallTests()
{
    // A private image big enough for the call site; the guard is written in.
    const size_t size = 0x1748000;
    auto* image = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    const unsigned char guard[] = {
        0x48,0x8b,0x85,0x08,0xf4,0xff,0xff, 0x48,0x83,0xec,0x08, 0x31,0xc9, 0x44,0x89,0xea,
        0x48,0xbe,0xff,0xff,0xff,0xff,0x00,0xff,0xff,0xff, 0x48,0x21,0xb5,0xc8,0xf3,0xff,0xff,
        0x4c,0x8b,0x85,0xa8,0xf3,0xff,0xff, 0x48,0x8b,0x78,0x40, 0x48,0x8b,0x85,0xc8,0xf3,0xff,0xff,
        0x6a,0x00, 0x48,0x8b,0xb5,0x18,0xf4,0xff,0xff, 0x49,0x89,0xc1, 0xe8,0x64,0xe2,0xda,0xff, 0x58,0x5a};
    memcpy(image + 0x17478d8, guard, sizeof(guard));
    assert(!mprotect(image, size, PROT_READ | PROT_EXEC));
    const char* id = "3a0e156390b0e6f1e372051c24802c8493ae454a";
    char path[] = "/tmp/town_trace_installXXXXXX";
    const int fd = mkstemp(path); assert(fd >= 0); close(fd);
    tpf2mp_town_trace::on.store(false);
    // Off unless exactly "1": nothing written.
    for (const char* v : {static_cast<const char*>(nullptr), "", "0", "true", "11"}) {
        if (v) setenv("TPF2MP_TOWN_TRACE", v, 1); else unsetenv("TPF2MP_TOWN_TRACE");
        assert(!Tpf2mpInstallTownTrace(base, id, path));
        assert(!strncmp(Tpf2mpTownTraceStatus(), "off (set TPF2MP_TOWN_TRACE=1", 28));
        assert(!memcmp(image + 0x17478d8, guard, sizeof(guard)) && !tpf2mp_town_trace::on.load());
    }
    setenv("TPF2MP_TOWN_TRACE", "1", 1);
    assert(!Tpf2mpInstallTownTrace(base, "wrong", path) && !strcmp(Tpf2mpTownTraceStatus(), "off (unverified image)"));
    // Any guard byte changed: refused, untouched.
    for (size_t at : {size_t(0), size_t(20), sizeof(guard) - 3}) {
        uint8_t x = image[0x17478d8 + at] ^ 1; int err = 0;
        assert(Tpf2mpCodeWriteSelf(base + 0x17478d8 + at, &x, 1, &err) == TPF2MP_CW_OK);
        assert(!Tpf2mpInstallTownTrace(base, id, path) && !strcmp(Tpf2mpTownTraceStatus(), "off (unverified TownSystem Develop call)"));
        x ^= 1; assert(Tpf2mpCodeWriteSelf(base + 0x17478d8 + at, &x, 1, &err) == TPF2MP_CW_OK);
        assert(!tpf2mp_town_trace::on.load());
    }
    assert(!Tpf2mpInstallTownTrace(base, id, "/nonexistent-dir/trace.txt") && !strcmp(Tpf2mpTownTraceStatus(), "off (trace file not writable)"));
    // Installed: the call now reaches a near stub that jumps to the wrapper.
    assert(Tpf2mpInstallTownTrace(base, id, path));
    assert(tpf2mp_town_trace::on.load() && !strncmp(Tpf2mpTownTraceStatus(), "enabled", 7));
    assert(image[0x1747917] == 0xe8);
    int32_t rel; memcpy(&rel, image + 0x1747918, 4);
    const uint8_t* stub = image + 0x174791c + rel;
    uintptr_t target; memcpy(&target, stub + 2, 8);
    assert(stub[0] == 0x48 && stub[1] == 0xb8 && stub[10] == 0xff && stub[11] == 0xe0);
    assert(target == reinterpret_cast<uintptr_t>(&Tpf2mpTownTraceDevelop));
    // The rest of the guard is as it was.
    assert(!memcmp(image + 0x17478d8, guard, 0x3f + 1) && !memcmp(image + 0x174791c, guard + 0x44, 2));
    assert(!Tpf2mpInstallTownTrace(base, id, path) && !strcmp(Tpf2mpTownTraceStatus(), "enabled (already installed)"));
    assert(ReadAll(path).rfind("# town trace, native build 35924, format 1\n", 0) == 0);
    unlink(path);
}

int main()
{
    HeaderTests();
    WrapperTests();
    InstallTests();
    printf("town trace: formats, Develop wrapper pass-through, family lines, install guard/refusals passed\n");
    return 0;
}
