// The company wash on the HUD station/depot icons (Linux Steam build 35924).
//
// This is the Linux counterpart of the Windows `stationicon` slice
// (native/src/slice_hook.cpp IconClassApply / IconOwnerForEntity). It arrives
// at the same visible result -- the icon glyph is painted in its owner's
// company colour -- but the mechanism is the Linux build's, not a translation
// of the Windows one, and the contracts the static-only runs left open were
// settled in the running game under gdb. See docs/re/linux/DEV_D6DB920F.md.
//
// How it works
//   * The icon element's carrier class ("train", "road", ...) is applied by
//     CComponent::addStyleClass 0x30550d0(widget, std::string*). There are
//     eleven such calls in StationItem and five in the depot item that
//     HudIconManager::DoStep inlines. Each one is redirected to
//     TintClassApply below, which runs the game's call first and then appends
//     "mpWinCo<company>" on the SAME element. The mod's stylesheet keys the
//     coloured glyph overlay on exactly that pair of classes.
//   * The entity whose icon is being built comes from the component lookup
//     each path performs just before: 0x109a8f0(engine, &entity, type) in the
//     StationItem constructor and 0x9e5590(engine, &entity, type) in DoStep's
//     depot branch. Redirecting those two calls records (engine, entity) for
//     the current thread and hands the call straight on. That covers both the
//     DoStep build and the cargo-state rebuild, because the station one sits
//     inside the constructor that both paths run.
//   * The owner is read WITHOUT the engine's asserting accessor: a HUD icon
//     entity may be a town or an industry, which has no PlayerOwned, and
//     0x9e5590 aborts on a missing component (Engine.h:291). The scan here is
//     the same one the port already uses for names, generalised over the
//     component stride and bounds-checked against the pool's own vector.
//
// What gdb proved in the lab (2026-09-17, save "Ordering desync Sep15"):
//   StationItem ctor 0x1090250: rsi = UI::EnginePtr, r9d = the entity.
//   0x146f0a0(&EnginePtr) returned the ecs engine.
//   Type indices from 0x9e3d50(engine+0x48, &type_info*): Name 19,
//   Player 18, PlayerOwned 52, StationGroup 55.
//   Icon entity 28301 ("Dinnington St John's Modular terminal #2") carried
//   StationGroup slot 1, Name slot 1344 AND PlayerOwned slot 7 -- so on Linux
//   the group entity itself is owned; the Windows group -> stations[0] walk is
//   only a fallback here. PlayerOwned slot 7 read with stride 4 gave entity
//   19427, which carries a Player component and the name
//   "ComradeSilver Transport". Its StationGroup vector held station 28300,
//   whose PlayerOwned was the same 19427.
//   The pool layout: engine+0x80 = pools, engine+0x98 = entity records,
//   pool+0xb8/+0xc0 = the component vector, pool+0xd0 = the page table. The
//   PlayerOwned stride of 4 is also fixed statically by the engine's own read
//   at 0x138babb / 0x138ba66 (`lea rax,[rdx+rax*4]`).
//
// Rendering only, main thread, once per icon build: it appends a style class
// and never touches simulation state, so it cannot desync.
// KILL SWITCH: `stationicon=0` in tpf2_menu_flags.txt. `tintclass=mpCo`
// switches the appended prefix to the opaque chip class, as on Windows.
#include "company_tint_linux.h"
#include "company_tint_checks_linux.h"
#include "ecs_linux.h"
#include "slice_core.h"
#include "../near_alloc.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

// ---- the ELF sites ---------------------------------------------------------
constexpr uintptr_t RVA_ADD_STYLE_CLASS = 0x30550d0;   // addStyleClass(widget, std::string*)
constexpr uintptr_t RVA_GET_COMPONENT   = 0x109a8f0;   // void* (engine, const int32*, int type)
constexpr uintptr_t RVA_GET_DATA_INDEX  = 0x9e5590;    // int   (engine, const int32*, int type)
constexpr uintptr_t SITE_STATION_CTX    = 0x10902e4;   // call 0x109a8f0 in StationItem::StationItem
constexpr uintptr_t SITE_DEPOT_CTX      = 0x1095525;   // call 0x9e5590 in HudIconManager::DoStep

uintptr_t tintBase;
std::string tintRoot, tintData;
bool tintOn;

// libstdc++ std::__cxx11::string, as the game's own libstdc++ lays it out.
struct GStr { char* p; size_t len; char buf[16]; };
static_assert(sizeof(GStr) == 32, "libstdc++ std::string layout");

using AddStyleFn   = void  (*)(void* widget, GStr* cls);
using GetCompFn    = void* (*)(void* engine, const int32_t* entity, int type);
using GetIndexFn   = int   (*)(void* engine, const int32_t* entity, int type);

std::atomic<uint64_t> asked{0}, tinted{0}, noOwner{0}, noCompany{0}, refused{0};
std::atomic<uint64_t> shown{0}, shownNoOwner{0};

// ---- flags -----------------------------------------------------------------
bool FlagSays(const char* key, const char* value)
{
    const std::string want = std::string(key) + "=" + value;
    for (const std::string& dir : {tintRoot, tintData}) {
        if (dir.empty()) continue;
        FILE* f = fopen((dir + "/tpf2_menu_flags.txt").c_str(), "r");
        if (!f) continue;
        char line[256];
        bool hit = false;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, want.c_str(), want.size())) hit = true;
        fclose(f);
        return hit;
    }
    return false;
}

// NO BANG: in the stylesheet "StationItem::StationIcon!train" the '!' is
// selector syntax; the class the element stores is "train". Ours is the same.
const char* ClassPrefix()
{
    static const char* cached = nullptr;
    if (!cached) cached = FlagSays("tintclass", "mpCo") ? "mpCo" : "mpWinCo";
    return cached;
}

// ---- pid -> company id, from the mod's mp_company_perms.txt ----------------
// Its own two-second cache, so it never disturbs the station-permission one.
int CompanyOfPid(int pid)
{
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static std::chrono::steady_clock::time_point last;
    static int count = 0;
    static int pids[256], cids[256];
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() == 0 || now - last >= std::chrono::seconds(2)) {
        last = now;
        count = 0;
        if (!tintData.empty()) {
            FILE* f = fopen((tintData + "/mp_company_perms.txt").c_str(), "r");
            if (f) {
                char line[160];
                while (fgets(line, sizeof(line), f)) {
                    int a = 0, b = 0;
                    if (sscanf(line, "pid %d %d", &a, &b) == 2 && count < 256) {
                        pids[count] = a; cids[count] = b; count++;
                    }
                }
                fclose(f);
            }
        }
    }
    for (int i = 0; i < count; ++i) if (pids[i] == pid) return cids[i];
    return 0;   // coop, or a player with no company: no wash
}

// ---- the icon being built --------------------------------------------------
// Set by the two context relays, read by the class relay, all on the UI thread
// within one icon build. Thread-local so a second UI thread cannot cross them.
thread_local uintptr_t tlEngine = 0;
thread_local int32_t tlEntity = -1;

// The widget's class list, read back so the log says whether ours landed.
void ClassList(uintptr_t widget, char* out, size_t cap)
{
    out[0] = 0;
    uintptr_t begin = 0, end = 0;
    if (!SliceReadT(widget + 0xb0, &begin) || !SliceReadT(widget + 0xb8, &end) ||
        !begin || end < begin || (end - begin) % 0x20 || (end - begin) > 0x20 * 64) {
        snprintf(out, cap, "(unreadable)");
        return;
    }
    size_t n = 0;
    for (uintptr_t r = begin; r < end && n + 2 < cap; r += 0x20) {
        std::string one;
        if (!SliceReadStdString(r, &one) || one.size() > 64) break;
        if (n) out[n++] = ' ';
        const size_t take = one.size() < cap - n - 1 ? one.size() : cap - n - 1;
        memcpy(out + n, one.data(), take);
        n += take;
        out[n] = 0;
    }
}

// "mpWinCo12" is always within the 15-byte local buffer, so the string owns no
// heap: addStyleClass either moves it (and clears ours) or drops a duplicate
// (and leaves ours alone). Either way there is nothing to free, and no
// allocator crosses between the game's libstdc++ and our static one.
void AppendClass(void* widget, int companyId)
{
    char text[32];
    snprintf(text, sizeof(text), "%s%d", ClassPrefix(), companyId);
    const size_t len = strlen(text);
    if (len >= sizeof(GStr::buf)) { refused.fetch_add(1); return; }
    GStr cls;
    cls.p = cls.buf;
    cls.len = len;
    memcpy(cls.buf, text, len + 1);
    reinterpret_cast<AddStyleFn>(tintBase + RVA_ADD_STYLE_CLASS)(widget, &cls);
}

} // namespace

// ---- the relays ------------------------------------------------------------
// Entered from the game through a near stub that clobbers only rax. Each one
// runs the original call and adds nothing to its contract.
extern "C" __attribute__((visibility("hidden")))
void* SliceTintStationContext(void* engine, const int32_t* entity, int type)
{
    if (entity) {
        int32_t id = -1;
        if (SliceRead(reinterpret_cast<uintptr_t>(entity), &id, sizeof(id))) {
            tlEngine = reinterpret_cast<uintptr_t>(engine);
            tlEntity = id;
        }
    }
    return reinterpret_cast<GetCompFn>(tintBase + RVA_GET_COMPONENT)(engine, entity, type);
}

extern "C" __attribute__((visibility("hidden")))
int SliceTintDepotContext(void* engine, const int32_t* entity, int type)
{
    if (entity) {
        int32_t id = -1;
        if (SliceRead(reinterpret_cast<uintptr_t>(entity), &id, sizeof(id))) {
            tlEngine = reinterpret_cast<uintptr_t>(engine);
            tlEntity = id;
        }
    }
    return reinterpret_cast<GetIndexFn>(tintBase + RVA_GET_DATA_INDEX)(engine, entity, type);
}

// The game's carrier class first, then the owner's company class on the same
// element. A throw from the original is the game's own and unwinds through
// here untouched; our part never throws out of this frame.
extern "C" __attribute__((visibility("hidden")))
void SliceTintClassApply(void* widget, void* cls)
{
    reinterpret_cast<AddStyleFn>(tintBase + RVA_ADD_STYLE_CLASS)(widget, static_cast<GStr*>(cls));
    asked.fetch_add(1);
    try {
        const uintptr_t engine = tlEngine;
        const int32_t entity = tlEntity;
        if (!widget || !engine || entity < 0) return;
        const int owner = SliceEcsOwner(engine, entity);
        if (owner < 0) {
            noOwner.fetch_add(1);
            if (shownNoOwner.fetch_add(1) < 4)
                SliceLog("[stationicon] entity %d has no owner (a town, an industry) -- untinted\n", entity);
            return;
        }
        const int company = CompanyOfPid(owner);
        if (company <= 0) { noCompany.fetch_add(1); return; }
        AppendClass(widget, company);
        tinted.fetch_add(1);
        if (shown.fetch_add(1) < 4) {
            char list[512];
            ClassList(reinterpret_cast<uintptr_t>(widget), list, sizeof(list));
            SliceLog("[stationicon] entity %d owner %d -> company %d: appended %s%d; classes now: %s\n",
                     entity, owner, company, ClassPrefix(), company, list);
        }
    } catch (...) {
        refused.fetch_add(1);   // only our own allocations; nothing foreign escapes
    }
}

// ---- install ---------------------------------------------------------------
namespace {
template <class T, size_t N> bool Anchored(uintptr_t base, const T (&checks)[N])
{
    for (const auto& c : checks) {
        std::vector<char> actual(c.size);
        if (!SliceRead(base + c.rva, actual.data(), actual.size()) ||
            memcmp(actual.data(), c.bytes, c.size)) {
            SliceLog("[stationicon] byte guard failed at %lx; the tint stays off\n",
                     (unsigned long)c.rva);
            return false;
        }
    }
    return true;
}
} // namespace

bool SliceInstallCompanyTint(uintptr_t base, const char* rootDir, const char* dataDir)
{
    tintBase = base;
    tintRoot = rootDir ? rootDir : "";
    tintData = dataDir ? dataDir : "";
    if (FlagSays("stationicon", "0")) {
        SliceLog("[stationicon] OFF (stationicon=0) -- HUD icons keep the game's colours\n");
        return true;
    }
    if (!Anchored(base, kTintContextChecks) || !Anchored(base, kTintStyleChecks) ||
        !SliceEcsAnchored(base)) return false;

    // The context relays go in first: a class call that fires before them just
    // sees no entity and leaves the icon alone.
    if (!Tpf2mpRedirectCall(base + SITE_STATION_CTX, base + RVA_GET_COMPONENT,
                            reinterpret_cast<void*>(&SliceTintStationContext))) {
        SliceLog("[stationicon] OFF: could not redirect the station entity lookup at %lx\n",
                 (unsigned long)SITE_STATION_CTX);
        return false;
    }
    if (!Tpf2mpRedirectCall(base + SITE_DEPOT_CTX, base + RVA_GET_DATA_INDEX,
                            reinterpret_cast<void*>(&SliceTintDepotContext))) {
        SliceLog("[stationicon] partially OFF: the depot entity lookup at %lx refused; "
                 "station icons still tint\n", (unsigned long)SITE_DEPOT_CTX);
    }
    size_t station = 0, depot = 0;
    for (uintptr_t site : kTintStationClassCalls)
        if (Tpf2mpRedirectCall(base + site, base + RVA_ADD_STYLE_CLASS,
                               reinterpret_cast<void*>(&SliceTintClassApply))) station++;
    for (uintptr_t site : kTintDepotClassCalls)
        if (Tpf2mpRedirectCall(base + site, base + RVA_ADD_STYLE_CLASS,
                               reinterpret_cast<void*>(&SliceTintClassApply))) depot++;
    tintOn = station > 0 || depot > 0;
    SliceLog("[stationicon] %s: %zu of %zu station and %zu of %zu depot carrier-class calls "
             "redirected; class prefix %s\n",
             tintOn ? "installed" : "OFF", station,
             sizeof(kTintStationClassCalls) / sizeof(kTintStationClassCalls[0]),
             depot, sizeof(kTintDepotClassCalls) / sizeof(kTintDepotClassCalls[0]), ClassPrefix());
    return tintOn;
}

void SliceCompanyTintLogAlive()
{
    if (!tintOn) return;
    SliceLog("[stationicon] alive: asked=%llu tinted=%llu noOwner=%llu noCompany=%llu refused=%llu\n",
             (unsigned long long)asked.load(), (unsigned long long)tinted.load(),
             (unsigned long long)noOwner.load(), (unsigned long long)noCompany.load(),
             (unsigned long long)refused.load());
}
