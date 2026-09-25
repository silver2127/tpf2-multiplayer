// libm parity: the UCRT float math models against golden hashes produced on
// Windows from ucrtbase.dll itself (windows_ucrt_math_golden_linux.h), vectors
// where glibc and ucrtbase measurably differ, and the installer's GOT/call-site
// verification, write and rollback on a fake image.
#include "../src/libm_parity_linux.cpp"
#include "../src/windows_ucrt_math_linux.h"
#include "windows_ucrt_math_golden_linux.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if __has_include(<bit>)
#include <bit>
#endif
#if defined(__cpp_lib_bit_cast)
static uint32_t B(float f) { return std::bit_cast<uint32_t>(f); }
static float F(uint32_t u) { return std::bit_cast<float>(u); }
#else
static uint32_t B(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static float F(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
#endif
static uint64_t Fnv(uint64_t h, uint32_t v)
{
    for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 0x100000001b3ULL; }
    return h;
}
template <class Fn> static uint64_t StrideHash(Fn fn)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint64_t i = 0; i < (1ULL << 32); i += kUcrtGoldenStride) h = Fnv(h, B(fn(F((uint32_t)i))));
    return h;
}

// ucrtbase (Windows) vs glibc 2.42 (the production server) on the same input:
// taken from the 200k-input libm comparison; the model must give the Windows bits.
struct Vec { const char* fn; uint32_t x, windows, glibc; };
static const Vec kVectors[] = {
    {"sinf", 0xc117f700, 0x3d956bfe, 0x3d956bff}, {"sinf", 0xc01bf5a4, 0xbf25d7d0, 0xbf25d7d1},
    {"sinf", 0x40be4a4e, 0xbea91c47, 0xbea91c48}, {"sinf", 0x410e0a48, 0x3f05360f, 0x3f05360e},
    {"cosf", 0xc105e3c7, 0xbefbca0b, 0xbefbca0a}, {"cosf", 0x3fae2998, 0x3e559d7a, 0x3e559d7b},
    {"cosf", 0xc09ca6bc, 0x3e3a4fd9, 0x3e3a4fda}, {"cosf", 0xc083fa72, 0xbf0e0459, 0xbf0e0458},
    {"tanf", 0xbf0dd22e, 0xbf1e5cfa, 0xbf1e5cfb}, {"tanf", 0x3f47bbe2, 0x3f7d5b8f, 0x3f7d5b90},
    {"tanf", 0x3f1087f6, 0x3f22229c, 0x3f22229d}, {"tanf", 0x3f0cf8d0, 0x3f1d310b, 0x3f1d310c},
    {"acosf", 0xbf50c016, 0x40218da2, 0x40218da3}, {"acosf", 0xbf29e6a2, 0x4012fa26, 0x4012fa27},
    {"acosf", 0xbf175212, 0x400d0172, 0x400d0173}, {"acosf", 0xbf519524, 0x4021ea04, 0x4021ea05},
    // Found by the exhaustive Windows check: ucrtbase's AVX2+FMA path differs
    // from its own SSE2 path here (sinf at 2^23 + 768, cosf of a tiny angle).
    {"sinf", 0x4b000300, 0xbf580632, 0},
    {"cosf", 0x3a544395, 0x3f7ffffa, 0},
};

static float Model(const char* fn, float x)
{
    if (!std::strcmp(fn, "sinf")) return Tpf2mpUcrtSinf(x);
    if (!std::strcmp(fn, "cosf")) return Tpf2mpUcrtCosf(x);
    if (!std::strcmp(fn, "tanf")) return Tpf2mpUcrtTanf(x);
    return Tpf2mpUcrtAcosf(x);
}
static float Glibc(const char* fn, float x)
{
    if (!std::strcmp(fn, "sinf")) return sinf(x);
    if (!std::strcmp(fn, "cosf")) return cosf(x);
    if (!std::strcmp(fn, "tanf")) return tanf(x);
    return acosf(x);
}

// ---- installer on a fake image ------------------------------------------------
static int FakeWrite(uintptr_t addr, const uint8_t* bytes, size_t len, int*)
{
    std::memcpy(reinterpret_cast<void*>(addr), bytes, len);
    return TPF2MP_CW_OK;
}
static int g_writes = 0, g_failAt = -1;
static int FailingWrite(uintptr_t addr, const uint8_t* bytes, size_t len, int* e)
{
    if (g_writes++ == g_failAt) return TPF2MP_CW_UNAVAILABLE;   // this one write fails, the rollback succeeds
    return FakeWrite(addr, bytes, len, e);
}
static uintptr_t FakeResolve(const char* name, uintptr_t current)
{
    return current == 0x1000u + (uintptr_t)std::strlen(name) ? current : 0;   // "libm" address per name
}
static std::vector<uintptr_t> g_redirected;
static bool FakeRedirect(uintptr_t site, uintptr_t callee, void* target)
{
    assert(target == reinterpret_cast<void*>(Tpf2mpUcrtAtan2FromFloats));
    const uint8_t* s = reinterpret_cast<const uint8_t*>(site);
    int32_t rel; std::memcpy(&rel, s + 1, 4);
    if (s[0] != 0xe8 || site + 5 + (intptr_t)rel != callee) return false;
    g_redirected.push_back(site);
    return true;
}
static uintptr_t BuildFakeImage(std::vector<unsigned char>& img)
{
    img.assign(0x5a48000, 0);
    const uintptr_t base = reinterpret_cast<uintptr_t>(img.data());
    for (const auto& s : g_slots) {
        const uintptr_t v = 0x1000u + std::strlen(s.slot.name);
        std::memcpy(&img[s.slot.rva], &v, 8);
    }
    for (const auto& site : kLibmAtan2Sites) std::memcpy(&img[site.guardRva], site.guard, site.guardSize);
    return base;
}
static void ResetModule()
{
    g_activeSlots = g_activeSites = 0;
    for (auto& s : g_slots) s.original = 0;
    g_redirected.clear();
}

int main()
{
    // 1. Golden: the models against ucrtbase.dll's own outputs.
    struct { const char* name; float (*fn)(float); uint64_t golden; } fns[] = {
        {"sinf", Tpf2mpUcrtSinf, kUcrtGolden_sinf}, {"cosf", Tpf2mpUcrtCosf, kUcrtGolden_cosf},
        {"tanf", Tpf2mpUcrtTanf, kUcrtGolden_tanf}, {"acosf", Tpf2mpUcrtAcosf, kUcrtGolden_acosf},
    };
    for (const auto& f : fns) {
        const uint64_t h = StrideHash(f.fn);
        std::printf("%-6s stride hash 0x%016llx (golden 0x%016llx)\n", f.name, (unsigned long long)h,
                    (unsigned long long)f.golden);
        assert(h == f.golden);
    }
    {
        uint64_t h = 0xcbf29ce484222325ULL, s = 0x243f6a8885a308d3ULL;
        for (int i = 0; i < (1 << 24); ++i) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            h = Fnv(h, B(Tpf2mpUcrtAtan2f(F((uint32_t)s), F((uint32_t)(s >> 32)))));
        }
        std::printf("atan2f pair hash 0x%016llx (golden 0x%016llx)\n", (unsigned long long)h,
                    (unsigned long long)kUcrtGolden_atan2f);
        assert(h == kUcrtGolden_atan2f);
    }
    // 2. Vectors where the platforms differ.
    unsigned glibcDiffers = 0;
    for (const auto& v : kVectors) {
        assert(B(Model(v.fn, F(v.x))) == v.windows);
        if (v.glibc && B(Glibc(v.fn, F(v.x))) != v.windows) ++glibcDiffers;
    }
    std::printf("vectors: model == ucrtbase for all %zu; this libm differs on %u of 16\n",
                sizeof(kVectors) / sizeof(kVectors[0]), glibcDiffers);
    // The atan2 call-site replacement narrows exactly.
    assert(Tpf2mpUcrtAtan2FromFloats((double)1.5f, (double)-2.25f) == (double)Tpf2mpUcrtAtan2f(1.5f, -2.25f));
    // sincosf is sinf + cosf.
    for (uint32_t u : {0x3f060a92u, 0x40490fdbu, 0x4b000300u, 0xc2c80000u}) {
        float s, c; Tpf2mpUcrtSincosf(F(u), &s, &c);
        assert(B(s) == B(Tpf2mpUcrtSinf(F(u))) && B(c) == B(Tpf2mpUcrtCosf(F(u))));
    }

    // 3. Installer.
    std::vector<unsigned char> img;
    uintptr_t base = BuildFakeImage(img);
    ResetModule();
    unsetenv("TPF2MP_LIBM_PARITY");
    assert(!InstallLibmParityWith(base, "not-the-build", FakeWrite, FakeResolve, FakeRedirect));
    setenv("TPF2MP_LIBM_PARITY", "0", 1);
    assert(!InstallLibmParityWith(base, kLibmBuildId, FakeWrite, FakeResolve, FakeRedirect));
    unsetenv("TPF2MP_LIBM_PARITY");
    // A foreign slot value refuses everything.
    { uintptr_t bad = 0x4242; std::memcpy(&img[kLibmGotCosf.rva], &bad, 8); }
    assert(!InstallLibmParityWith(base, kLibmBuildId, FakeWrite, FakeResolve, FakeRedirect));
    assert(std::strstr(Tpf2mpLibmParityStatus(), "cosf"));
    base = BuildFakeImage(img);
    ResetModule();
    // A changed guard byte refuses everything, before any write.
    img[kLibmAtan2Sites[1].guardRva + 2] ^= 1;
    assert(!InstallLibmParityWith(base, kLibmBuildId, FakeWrite, FakeResolve, FakeRedirect));
    { uintptr_t v; std::memcpy(&v, &img[kLibmGotSinf.rva], 8); assert(v == 0x1000u + 4); }
    base = BuildFakeImage(img);
    ResetModule();
    // Success: every slot points at the model, every site redirected.
    assert(InstallLibmParityWith(base, kLibmBuildId, FakeWrite, FakeResolve, FakeRedirect));
    for (const auto& s : g_slots) {
        uintptr_t v; std::memcpy(&v, &img[s.slot.rva], 8);
        assert(v == reinterpret_cast<uintptr_t>(s.replacement));
    }
    assert(g_redirected.size() == kLibmAtan2SiteCount);
    assert(std::strncmp(Tpf2mpLibmParityStatus(), "enabled", 7) == 0);
    // Installed slots call the models.
    { uintptr_t v; std::memcpy(&v, &img[kLibmGotAcosf.rva], 8);
      assert(B(reinterpret_cast<float (*)(float)>(v)(F(0xbf50c016u))) == 0x40218da2u); }
    // A second install changes nothing.
    assert(!InstallLibmParityWith(base, kLibmBuildId, FakeWrite, FakeResolve, FakeRedirect));
    // A failed write rolls back the slots already written.
    base = BuildFakeImage(img);
    ResetModule();
    g_writes = 0; g_failAt = 3;
    assert(!InstallLibmParityWith(base, kLibmBuildId, FailingWrite, FakeResolve, FakeRedirect));
    g_failAt = -1;
    for (const auto& s : g_slots) {
        uintptr_t v; std::memcpy(&v, &img[s.slot.rva], 8);
        assert(v == 0x1000u + std::strlen(s.slot.name));
    }
    assert(g_activeSlots == 0 && g_redirected.empty());
    std::printf("libm parity: all checks passed\n");
    return 0;
}
