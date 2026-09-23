// libm_parity_linux.cpp -- Windows (UCRT) float math for the native game.
//
// Evidence (docs/re/crossplatform/STREET_LIBM_REPORT.md):
//   * The town street developer (Windows 0x140986070 / native 0x1579a20 and
//     their callees) and TownDeveloper::Develop feed sinf/cosf/tanf/acosf/
//     atan2f results into street geometry, angle thresholds and int
//     truncations. glibc 2.42 and ucrtbase differ in 1.2-1.9% of sinf/cosf/
//     acosf results and 0.03% of tanf results on game-like inputs.
//   * GCC resolved ::atan2(float, float) to the double atan2 at three
//     street developer sites where the Windows functions call atan2f.
//   * windows_ucrt_math_linux.cpp reproduces ucrtbase.dll bit for bit over
//     all 2^32 inputs (sinf/cosf/tanf/acosf) and 2^31 atan2f pairs.
#include "libm_parity_linux.h"
#include "libm_parity_sites_linux.h"
#include "windows_ucrt_math_linux.h"
#include "codewrite_linux.h"
#include "near_alloc.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" __attribute__((visibility("hidden")))
double Tpf2mpUcrtAtan2FromFloats(double y, double x)
{
    // The site converted two floats to double (exact); narrowing is exact too.
    return (double)Tpf2mpUcrtAtan2f((float)y, (float)x);
}

namespace {
constexpr char kLibmBuildId[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";
char g_status[224] = "off (not initialized)";
unsigned g_activeSlots = 0, g_activeSites = 0;

struct SlotPlan { Tpf2mpGotSlot slot; void* replacement; uintptr_t original; };

float SinfEntry(float x) { return Tpf2mpUcrtSinf(x); }
float CosfEntry(float x) { return Tpf2mpUcrtCosf(x); }
void SincosfEntry(float x, float* s, float* c) { Tpf2mpUcrtSincosf(x, s, c); }
float TanfEntry(float x) { return Tpf2mpUcrtTanf(x); }
float AcosfEntry(float x) { return Tpf2mpUcrtAcosf(x); }
float Atan2fEntry(float y, float x) { return Tpf2mpUcrtAtan2f(y, x); }

SlotPlan g_slots[] = {
    {kLibmGotSinf, reinterpret_cast<void*>(SinfEntry), 0},
    {kLibmGotCosf, reinterpret_cast<void*>(CosfEntry), 0},
    {kLibmGotSincosf, reinterpret_cast<void*>(SincosfEntry), 0},
    {kLibmGotTanf, reinterpret_cast<void*>(TanfEntry), 0},
    {kLibmGotAcosf, reinterpret_cast<void*>(AcosfEntry), 0},
    {kLibmGotAtan2f, reinterpret_cast<void*>(Atan2fEntry), 0},
};
constexpr unsigned kSlotCount = sizeof(g_slots) / sizeof(g_slots[0]);

using Writer = int (*)(uintptr_t, const uint8_t*, size_t, int*);
using Resolver = uintptr_t (*)(const char*, uintptr_t current);
using CallRedirect = bool (*)(uintptr_t site, uintptr_t expectedCallee, void* target);

// The slot must already hold libm's function of that name: dlsym of the name,
// or (for a versioned import) an address dladdr attributes to that symbol.
uintptr_t ResolveLibm(const char* name, uintptr_t current)
{
    void* sym = dlsym(RTLD_DEFAULT, name);
    if (sym && reinterpret_cast<uintptr_t>(sym) == current) return current;
    Dl_info info{};
    if (current && dladdr(reinterpret_cast<void*>(current), &info) && info.dli_sname &&
        std::strcmp(info.dli_sname, name) == 0 && info.dli_fname && std::strstr(info.dli_fname, "libm"))
        return current;
    return 0;
}

void Rollback(uintptr_t base, unsigned slots, Writer write)
{
    for (unsigned i = 0; i < kSlotCount; ++i) {
        if (!(slots & (1u << i))) continue;
        int error = 0;
        const uintptr_t orig = g_slots[i].original;
        write(base + g_slots[i].slot.rva, reinterpret_cast<const uint8_t*>(&orig), sizeof(orig), &error);
        uintptr_t now;
        std::memcpy(&now, reinterpret_cast<void*>(base + g_slots[i].slot.rva), sizeof(now));
        if (now == orig) g_activeSlots &= ~(1u << i);
    }
}

bool InstallLibmParityWith(uintptr_t base, const char* buildId, Writer write, Resolver resolve,
                           CallRedirect redirect)
{
    if (g_activeSlots || g_activeSites) {
        std::snprintf(g_status, sizeof(g_status), "unchanged (active: slots 0x%x, sites 0x%x)",
                      g_activeSlots, g_activeSites);
        return false;
    }
    if (const char* kill = std::getenv("TPF2MP_LIBM_PARITY"))
        if (std::strcmp(kill, "0") == 0) { std::strcpy(g_status, "off (TPF2MP_LIBM_PARITY=0)"); return false; }
    if (!base || !buildId || std::strcmp(buildId, kLibmBuildId) != 0) {
        std::strcpy(g_status, "off (unverified image)"); return false;
    }
    // Verify everything before writing anything.
    for (auto& s : g_slots) {
        uintptr_t current;
        std::memcpy(&current, reinterpret_cast<void*>(base + s.slot.rva), sizeof(current));
        s.original = resolve(s.slot.name, current);
        if (!s.original) {
            std::snprintf(g_status, sizeof(g_status), "off (GOT slot %s at 0x%lx is not libm's)",
                          s.slot.name, (unsigned long)s.slot.rva);
            return false;
        }
    }
    for (const auto& site : kLibmAtan2Sites) {
        if (std::memcmp(reinterpret_cast<void*>(base + site.guardRva), site.guard, site.guardSize) != 0) {
            std::snprintf(g_status, sizeof(g_status), "off (unverified atan2 site 0x%lx)",
                          (unsigned long)site.callRva);
            return false;
        }
    }
    unsigned slots = 0;
    for (unsigned i = 0; i < kSlotCount; ++i) {
        int error = 0;
        const uintptr_t value = reinterpret_cast<uintptr_t>(g_slots[i].replacement);
        const int r = write(base + g_slots[i].slot.rva, reinterpret_cast<const uint8_t*>(&value),
                            sizeof(value), &error);
        slots |= 1u << i;   // a failed writer may have written a prefix
        g_activeSlots = slots;
        if (r != TPF2MP_CW_OK) {
            Rollback(base, slots, write);
            std::snprintf(g_status, sizeof(g_status), g_activeSlots
                ? "ERROR: libm parity rollback incomplete (slots 0x%x)" : "off (GOT write %u failed; restored)",
                g_activeSlots ? g_activeSlots : i);
            return false;
        }
    }
    // Call sites: a failure here keeps the GOT redirects (they are correct on
    // their own) and reports which sites stayed on the double atan2.
    unsigned sites = 0;
    for (unsigned i = 0; i < kLibmAtan2SiteCount; ++i)
        if (redirect(base + kLibmAtan2Sites[i].callRva, base + kLibmPltAtan2Rva,
                     reinterpret_cast<void*>(Tpf2mpUcrtAtan2FromFloats)))
            sites |= 1u << i;
    g_activeSites = sites;
    if (sites == (1u << kLibmAtan2SiteCount) - 1)
        std::strcpy(g_status, "enabled: UCRT sinf/cosf/sincosf/tanf/acosf/atan2f (GOT) + 3 street developer atan2f sites");
    else
        std::snprintf(g_status, sizeof(g_status),
                      "partial: UCRT sinf/cosf/sincosf/tanf/acosf/atan2f (GOT); atan2 sites 0x%x of 0x%x",
                      sites, (1u << kLibmAtan2SiteCount) - 1);
    return true;
}
} // namespace

bool Tpf2mpInstallLibmParity(uintptr_t imageBase, const char* buildId)
{
    return InstallLibmParityWith(imageBase, buildId, Tpf2mpCodeWriteSelf, ResolveLibm, Tpf2mpRedirectCall);
}

const char* Tpf2mpLibmParityStatus() { return g_status; }
