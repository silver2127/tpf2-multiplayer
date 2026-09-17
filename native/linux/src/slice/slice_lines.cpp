// Linux build 35924: layouts/callback policies from docs/re/linux/SLICE_LINES.md.
#include "slice_core.h"
#include "ecs_linux.h"
#include <vector>
#include <cmath>
#include <cerrno>
#include <initializer_list>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

namespace slice_lines {
constexpr uintptr_t kSet = 0x15ed550, kCreate = 0x15efda0, kUpdate = 0x15f0050;
constexpr uintptr_t kDelete = 0x15ebd00, kColor = 0x15ecb40, kName = 0x15ee6d0;

// Byte-for-byte native/src/slice_hook.cpp's encoder: the wire splits records on
// whitespace, and a company is called "Ferrovie dello Stato". '%' and '=' are
// escaped too, so the encoding round-trips and never collides with a key=value.
std::string PercentEncode(const std::string& s)
{
    std::string enc;
    enc.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch > 32 && ch < 127 && ch != '%' && ch != '=') enc.push_back((char)ch);
        else { char h[4]; snprintf(h, sizeof(h), "%%%02X", ch); enc.append(h, 3); }
    }
    return enc;
}

// Layouts rechecked against build 35924; see SLICE_LINES.md. No game-size
// policy limits: only vector structure/readability, valid IDs/mode and non-NaN waits.
static bool Refuse(const char* check, size_t stop = 0)
{
    SliceLog("[slice-lines] decode refused: %s (stop %zu)\n", check, stop);
    return false;
}
static bool Wait(uintptr_t address, float* value)
{
    return SliceReadT(address, value) && !std::isnan(*value);
}

static bool DecodeBody(SliceRecord* rec, uintptr_t line)
{
    float wait;
    SliceVec stops;
    if (!Wait(line + 0x18, &wait)) return Refuse("waitingTime unreadable or NaN");
    if (!SliceReadStdVector(line, 0xb8, SIZE_MAX, &stops)) return Refuse("stops vector");
    SliceRecordPrintf(rec, "%.9g %zu", wait, stops.count);
    for (size_t i = 0; i < stops.count; ++i) {
        const uintptr_t s = stops.begin + i * 0xb8;
        int32_t ids[3], mode;
        float min, max;
        SliceVec alts;
        if (!SliceRead(s, ids, sizeof(ids)) || ids[0] <= 0 || ids[1] < 0 || ids[2] < 0)
            return Refuse("station group/station/terminal", i + 1);
        if (!SliceReadT(s + 0x28, &mode) || mode < 0 || mode > 3) return Refuse("loadMode", i + 1);
        if (!Wait(s + 0x2c, &min) || !Wait(s + 0x30, &max)) return Refuse("stop waits unreadable or NaN", i + 1);
        if (!SliceReadStdVector(s + 0x10, 8, SIZE_MAX, &alts)) return Refuse("alternative terminals vector", i + 1);
        SliceRecordPrintf(rec, " %d %d %d %d %.9g %.9g %zu", ids[0], ids[1], ids[2], mode, min, max, alts.count);
        for (size_t a = 0; a < alts.count; ++a) {
            int32_t idsAlt[2];
            if (!SliceRead(alts.begin + a * 8, idsAlt, sizeof(idsAlt)) || idsAlt[0] < 0 || idsAlt[1] < 0)
                return Refuse("alternative station/terminal", i + 1);
            SliceRecordPrintf(rec, " %d %d", idsAlt[0], idsAlt[1]);
            if (rec->failed) return Refuse("record allocation", i + 1);
        }
    }
    bool first = true;
    for (size_t i = 0; i < stops.count; ++i) {
        SliceVec points;
        if (!SliceReadStdVector(stops.begin + i * 0xb8 + 0x38, 8, SIZE_MAX, &points)) return Refuse("waypoints vector", i + 1);
        for (size_t j = 0; j < points.count; ++j) {
            int32_t id[2];
            if (!SliceRead(points.begin + j * 8, id, sizeof(id)) || id[0] <= 0 || id[1] < 0)
                return Refuse("waypoint entity/index", i + 1);
            SliceRecordPrintf(rec, "%s%zu:%d:%d", first ? " wp=" : ",", i + 1, id[0], id[1]);
            first = false;
            if (rec->failed) return Refuse("record allocation", i + 1);
        }
    }
    return !rec->failed || Refuse("record allocation");
}

static bool Decode(SliceRecord* rec, int32_t entity, uintptr_t line)
{
    SliceRecordPrintf(rec, "LUPDATE %d ", entity);
    return DecodeBody(rec, line);
}

static bool EncodeName(SliceRecord* rec, uintptr_t address)
{
    std::string name;
    if (!SliceReadStdString(address, &name) || name.empty()) return false;
    const size_t len = name.size();
    for (size_t i = 0; i < len; ++i) {
        const unsigned char ch = name[i];
        if (ch > 32 && ch < 127 && ch != '%' && ch != '=') SliceRecordAppend(rec, name.data() + i, 1);
        else SliceRecordPrintf(rec, "%%%02X", unsigned(ch));
    }
    return !rec->failed;
}

static bool DecodeCreate(SliceRecord* rec, uintptr_t name, const float* rgb, uintptr_t line)
{
    for (int i = 0; i < 3; ++i) if (!(rgb[i] >= 0 && rgb[i] <= 1)) return false;
    SliceRecordPrintf(rec, "LCREATEX %.9g %.9g %.9g ", rgb[0], rgb[1], rgb[2]);
    if (!DecodeBody(rec, line)) return false;
    SliceRecordPrintf(rec, " name=");
    if (!EncodeName(rec, name)) return false;
    SliceRecordPrintf(rec, "\n");
    return !rec->failed;
}

// release-0.4.22 strict line creation. These are libstdc++'s relocatable 16-byte
// _Any_data followed by manager/invoker, not the 64-byte Windows function object.
struct HeldCreate {
    alignas(16) uintptr_t fn[4];
    uint64_t id, at, claimBefore, claim;
    char instance[8];
    SliceRecord identity;
    bool inflight;
};
static std::vector<HeldCreate*> g_creates;
static pthread_mutex_t g_createMutex = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<uint32_t> g_createSequence{0};
static uint64_t g_claimSeen = 0;
static char g_claimInstance[8]{};
static void* g_directSinkTrampoline = nullptr;
struct CreatePending { SliceRecord record; uint64_t id; };
static thread_local CreatePending t_create{};
struct Carrier { uint64_t id, at; char instance[8]; };
static thread_local Carrier t_carrier{};

static bool WriteMemory(uintptr_t address, const void* bytes, size_t size)
{
    iovec src{const_cast<void*>(bytes),size}, dst{reinterpret_cast<void*>(address),size};
    return process_vm_writev(getpid(), &src, 1, &dst, 1, 0) == ssize_t(size);
}

static void ReleaseCreate(void*, void* context)
{
    auto* held = static_cast<HeldCreate*>(context);
    pthread_mutex_lock(&g_createMutex);
    for (auto& slot : g_creates) if (slot == held) slot = nullptr;
    pthread_mutex_unlock(&g_createMutex);
    // Add moves and empties the function; the immediate sink only invokes it.
    // A std::function manager's destroy operation destroys its noexcept functor.
    if (held->fn[2]) reinterpret_cast<void (*)(void*, const void*, int)>(held->fn[2])(held->fn, held->fn, 3);
    SliceRecordFree(&held->identity);
    std::free(held);
}

static void ExpireCreates()
{
    std::vector<HeldCreate*> expired;
    const uint64_t now=SliceNowMs();
    pthread_mutex_lock(&g_createMutex);
    try { expired.reserve(g_creates.size()); } catch (...) { pthread_mutex_unlock(&g_createMutex); throw; }
    for (auto& slot:g_creates) if (slot && !slot->inflight && now-slot->at>60000) {
        expired.push_back(slot);slot=nullptr;
    }
    pthread_mutex_unlock(&g_createMutex);
    for (auto* h : expired) ReleaseCreate(nullptr,h);
}

static bool FreshClaim(uint64_t* nonce, char* letter)
{
    if (!SliceInstance(letter,8)) return false;
    char path[4096];
    const int n = std::snprintf(path,sizeof(path),"%slockstep_lclaim_%s.txt",SliceDataDir(),letter);
    if (n < 0 || size_t(n) >= sizeof(path)) return false;
    const int fd = open(path,O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat statbuf{}; struct timespec now{}; char data[96]{};
    const bool statOk = fstat(fd,&statbuf) == 0 && clock_gettime(CLOCK_REALTIME,&now) == 0;
    const int64_t age = statOk ? int64_t(now.tv_sec-statbuf.st_mtim.tv_sec)*1000000000 +
        now.tv_nsec-statbuf.st_mtim.tv_nsec : -1;
    const bool fresh = statOk && age >= 0 && age <= 5000000000LL && statbuf.st_size > 0 &&
        statbuf.st_size < off_t(sizeof(data));
    const ssize_t count = fresh ? read(fd,data,sizeof(data)-1) : -1;
    close(fd);
    if (count <= 0 || count != statbuf.st_size || data[0] < '0' || data[0] > '9') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(data,&end,10);
    if (errno || !value || !end) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end) return false;
    *nonce = value;
    return true;
}

static bool SameCreate(const HeldCreate& held, uintptr_t name, const float* rgb, uintptr_t line)
{
    SliceRecord rec{};
    const bool same = DecodeCreate(&rec,name,rgb,line) && rec.len == held.identity.len &&
        std::memcmp(rec.data,held.identity.data,rec.len) == 0;
    SliceRecordFree(&rec);
    return same;
}

static void ClaimCreate(const SliceFactoryCall& c)
{
    // A preceding maker which threw or never reached a sink cannot lend its
    // reservation to another create. No game-owned object is destroyed here.
    pthread_mutex_lock(&g_createMutex);
    for (auto* h : g_creates) if (h && h->id == t_carrier.id && !h->inflight) h->claim = 0;
    pthread_mutex_unlock(&g_createMutex);
    t_carrier = {};
    uint64_t nonce; char instance[8];
    if (!SliceSessionLive() || !FreshClaim(&nonce,instance)) return;
    const float rgb[] = {SliceXmmFloat(c,0,0),SliceXmmFloat(c,0,1),SliceXmmFloat(c,1,0)};
    pthread_mutex_lock(&g_createMutex);
    // The unchanged 0.4.22 Lua writes one fresh, clock-seeded lclaimSeq per
    // origin replay. Consume it even if its payload has no matching holder, so
    // a later peer's identical create cannot reuse that claim.
    if (std::strcmp(g_claimInstance,instance)) {
        std::strcpy(g_claimInstance,instance); g_claimSeen = 0;
    }
    HeldCreate* oldest = nullptr;
    if (nonce != g_claimSeen) for (auto* h : g_creates) if (h && !h->inflight && !h->claim &&
        nonce != h->claimBefore && std::strcmp(h->instance,instance) == 0 &&
        (!oldest || h->id < oldest->id) && SameCreate(*h,c.rsi,rgb,c.rcx)) oldest = h;
    g_claimSeen = nonce;
    if (oldest) {
        oldest->claim = nonce;
        t_carrier = {oldest->id,SliceNowMs(),{}};
        std::strcpy(t_carrier.instance,instance);
    }
    pthread_mutex_unlock(&g_createMutex);
}

static HeldCreate* TakeCarrier(uintptr_t commandAddress)
{
    if (!t_carrier.id) return nullptr;
    const Carrier carrier = t_carrier;
    t_carrier = {};
    char instance[8]; uintptr_t impl; float rgb[3];
    const bool valid = SliceNowMs()-carrier.at <= 5000 && SliceSessionLive() && SliceInstance(instance,sizeof(instance)) &&
        !std::strcmp(carrier.instance,instance) && SliceCommandTag(commandAddress) == 3 &&
        SliceReadT(commandAddress,&impl) && SliceRead(impl+0x50,rgb,sizeof(rgb));
    // The script factory's Command was destroyed and rebuilt by sendCommand.
    // Match the reserved holder AND normalized payload, never its obsolete pointer.
    HeldCreate* found = nullptr;
    pthread_mutex_lock(&g_createMutex);
    for (auto* h : g_creates) if (h && !h->inflight && h->id == carrier.id) {
        if (valid && h->claim && std::strcmp(h->instance,instance) == 0 && SameCreate(*h,impl+0x30,rgb,impl)) {
            found = h; h->inflight = true;
        } else h->claim = 0;
        break;
    }
    pthread_mutex_unlock(&g_createMutex);
    return found;
}

// Windows dev 1d0ca473 converges on this existing Linux scheme: sendCommand
// rebuilds the maker's Command. The two verified script callers plus the
// thread-local claim identify the replay; TakeCarrier also checks its payload.
// See docs/re/linux/DEV_1D0CA473.md for the rechecked ELF/ABI evidence.
static void ReplayAdd(const SliceAddCall& add, void*)
{
    if (!t_carrier.id || (add.retRva != 0xa2f5c2 && add.retRva != 0x11225a9)) return;
    HeldCreate* h = TakeCarrier(uintptr_t(add.cmd));
    if (!h) return;
    if (!SliceOverrideAddDone(add,h->fn,ReleaseCreate,h)) {
        pthread_mutex_lock(&g_createMutex); h->inflight=false; h->claim=0; pthread_mutex_unlock(&g_createMutex);
    }
}

// The simulation script state's sink applies directly without CommandList::Add.
// Preserve its original move/apply/invoke/destruction sequence, substituting only
// the callback. No C++ cleanup surrounds game code; an exception retains h safely.
static void DirectSink(void* functor, void* commandArg, void* done)
{
    HeldCreate* h = TakeCarrier(uintptr_t(commandArg));
    reinterpret_cast<void (*)(void*,void*,void*)>(g_directSinkTrampoline)(functor,commandArg,h ? h->fn : done);
    if (h) ReleaseCreate(h->fn,h);
}

static void CreateLanded(const SliceAddCall*, SliceOutcome, void*)
{
    SliceRecordFree(&t_create.record);
    t_create = {};
}

static bool PrepareCreate(const SliceAddCall& add, void*)
{
    ExpireCreates();
    uintptr_t fn[4]; char instance[8];
    if (!SliceSessionLive() || !SliceInstance(instance,sizeof(instance)) ||
        !SliceRead(uintptr_t(add.done),fn,sizeof(fn)) ||
        !((fn[2] == SliceAddr(0x10d44b0) && fn[3] == SliceAddr(0x10d6c30)) ||
          (fn[2] == SliceAddr(0x10d8db0) && fn[3] == SliceAddr(0x10e1a60)))) return false;
    auto* h = static_cast<HeldCreate*>(std::calloc(1,sizeof(HeldCreate)));
    if (!h) return false;
    std::memcpy(h->fn,fn,sizeof(fn)); h->id=t_create.id; h->at=SliceNowMs(); std::strcpy(h->instance,instance);
    char claimInstance[8]; uint64_t priorClaim;
    if (FreshClaim(&priorClaim,claimInstance) && !std::strcmp(claimInstance,instance)) h->claimBefore=priorClaim;
    size_t slot=0;
    // A simulation-thread replay can observe the append immediately. Publish
    // the moved callback and its identity in the same critical section as the
    // append, so that replay cannot claim a half-committed holder.
    pthread_mutex_lock(&g_createMutex);
    for (;slot<g_creates.size();++slot) if (!g_creates[slot]) break;
    try { if (slot == g_creates.size()) g_creates.push_back(nullptr); }
    catch (...) { pthread_mutex_unlock(&g_createMutex); std::free(h); throw; }
    g_creates[slot]=h;
    const uintptr_t zero=0;
    if (!WriteMemory(uintptr_t(add.done)+0x10,&zero,sizeof(zero))) {
        g_creates[slot]=nullptr;pthread_mutex_unlock(&g_createMutex);
        h->fn[2]=0;ReleaseCreate(nullptr,h);return false;
    }
    const auto result = SliceInjectWrite(t_create.record,SliceArmedLine::One);
    if (result == SliceInjectResult::NotWritten) {
        if (!WriteMemory(uintptr_t(add.done)+0x10,&fn[2],sizeof(fn[2]))) {
            SliceLog("[slice-lines] CreateLine callback restore failed after an unwritten record\n");
            h->identity=t_create.record;t_create.record={};
            h->claim=UINT64_MAX; // No capture exists; retain only for safe expiry.
            pthread_mutex_unlock(&g_createMutex);
            return false; // Retain ownership for expiry; the caller's manager is still empty.
        }
        g_creates[slot]=nullptr;pthread_mutex_unlock(&g_createMutex);
        h->fn[2]=0;ReleaseCreate(nullptr,h);return false;
    }
    h->identity=t_create.record; t_create.record={};
    pthread_mutex_unlock(&g_createMutex);
    return true;
}

struct Pending { bool active; SliceFactoryCall call; SliceRecord record; uintptr_t blockedCounter; };
static thread_local Pending t_counter{};

static void Landed(const SliceAddCall* add, SliceOutcome outcome, void*)
{
    // This callback normally decrements the pre-factory increment. BlockAdd
    // never invokes game callbacks; release only the counter verified during
    // this exact Add's prepare pass, while its callback still owns the storage.
    if (outcome == SliceOutcome::Blocked && add && uintptr_t(add->cmd) == t_counter.call.rdi &&
        t_counter.blockedCounter) {
        int32_t count;
        if (SliceReadT(t_counter.blockedCounter,&count) && count > 0) {
            --count;
            if (!WriteMemory(t_counter.blockedCounter,&count,sizeof(count)))
                SliceLog("[slice-lines] blocked UpdateLine counter could not be released\n");
        }
    }
    SliceRecordFree(&t_counter.record);
    t_counter = {};
}

static bool PrepareCounter(const SliceAddCall& add, void*)
{
    if (!t_counter.active || uintptr_t(add.cmd) != t_counter.call.rdi) return false;
    uintptr_t manager, invoker, lambda, counter;
    int32_t count;
    const bool safe = SliceSessionLive() && SliceCancelAvailable() && SliceCommandTag(uintptr_t(add.cmd)) == 5 &&
        SliceStdFunctionParts(uintptr_t(add.done), &manager, &invoker) &&
        manager == SliceAddr(0x1323eb0) && invoker == SliceAddr(0x13228e0) &&
        SliceReadT(uintptr_t(add.done), &lambda) && SliceReadT(lambda, &counter) &&
        SliceReadT(counter, &count) && count > 0;
    t_counter.blockedCounter = safe ? counter : 0;
    return safe && SliceInjectWrite(t_counter.record,SliceArmedLine::One) != SliceInjectResult::NotWritten;
}

static void OnFactory(const SliceFactoryCall& c, void*)
{
    if (c.factory->rva == kCreate && c.script) { ClaimCreate(c); return; }
    if (c.script || !c.armable || !SliceSessionLive()) return;
    SliceRecord rec{};
    const int32_t entity = int32_t(c.rdx);
    bool cancel = false, valid = true;
    const char* what = "line";
    switch (c.factory->rva) {
        case kSet:
            what = "VLINE"; cancel = true;
            SliceRecordPrintf(&rec, "VLINE %d %d %d", entity, int32_t(c.rcx), int32_t(c.r8));
            break;
        case kCreate: {
            what = "LCREATE";
            const uint64_t id=(uint64_t(uint32_t(getpid()))<<32) | ++g_createSequence;
            const float rgb[]={SliceXmmFloat(c,0,0),SliceXmmFloat(c,0,1),SliceXmmFloat(c,1,0)};
            if (c.retRva == 0x2eb1c49 && SliceHookInstalled(0xa2d650) && SliceSessionLive() && SliceCancelAvailable() &&
                DecodeCreate(&rec,c.rsi,rgb,c.rcx) &&
                SliceArmCancel(c,{"LCREATEX",SliceDone::Never,true,nullptr,CreateLanded,nullptr,PrepareCreate})) {
                t_create={rec,id};return;
            }
            SliceRecordFree(&rec);
            return;
        }
        case kUpdate:
            what = "LUPDATE";
            cancel = Decode(&rec, entity, c.rcx);
            if (!cancel) {
                SliceRecordFree(&rec);
                return;
            }
            break;
        case kDelete:
            what = "LDELETE"; cancel = true;
            SliceRecordPrintf(&rec, "LDELETE %d", entity);
            break;
        case kName: {
            // THE COMPANY WINDOW'S RENAME (Windows 2a87bb4's Linux half).
            // A VNAME whose entity is a company -- an entity with a Player
            // component -- is turned into CMNAME by the shared inject.lua, and
            // CMNAME renames the company on EVERY peer, the originator
            // included. So a cancelled click is not lost here the way a
            // vehicle's or a line's rename would be, and the barrier's replay
            // adapter is not needed. Everything else keeps the old refusal.
            // rsi = the engine and rdx = the entity at this factory: watched
            // under gdb in the lab (entity 19427, Player yes, PlayerOwned no).
            if (!SliceEcsIsCompany(c.rsi, entity)) {
                SliceLog("[slice-lines] VNAME blocked for entity %d: not a company, and unchanged "
                         "0.4.22 Lua skips origin replay\n", entity);
                return;
            }
            std::string name;
            if (!SliceReadStdString(c.rcx, &name) || name.empty()) {
                SliceLog("[slice-lines] VNAME for company %d: the name did not read, or is "
                         "empty -- not shipped\n", entity);
                return;
            }
            what = "VNAME"; cancel = true;
            SliceRecordPrintf(&rec, "VNAME %d %s", entity, PercentEncode(name).c_str());
            break;
        }
        case kColor:
            // Unchanged Windows 0.4.22 Lua unconditionally sets skipOrigin=1
            // for these records. Shipping a cancelled click would apply only
            // on peers. The central player barrier blocks it until a native
            // origin replay adapter is available; script replays pass above.
            SliceLog("[slice-lines] VCOLOR blocked: unchanged 0.4.22 Lua skips origin replay\n");
            return;
        default: return;
    }
    SliceRecordPrintf(&rec, "\n");
    if (!valid || rec.failed) { SliceRecordFree(&rec); return; }
    // This one callback balances a counter increment made BEFORE the factory.
    // Recognize it at Add and require it to fire; other line callbacks stay silent.
    if (cancel && c.factory->rva == kUpdate && c.retRva == 0x132c513) {
        if (SliceSessionLive() && SliceCancelAvailable() &&
            SliceArmCancel(c, {what, SliceDone::Required, true, nullptr, Landed, nullptr, PrepareCounter})) {
            t_counter = {true, c, rec, 0};
            return;
        }
        SliceRecordFree(&rec);
        return;
    }
    SliceShipAndArm(c, {what, SliceDone::Never, false, nullptr, nullptr, nullptr}, rec);
    SliceRecordFree(&rec);
}
} // namespace slice_lines

SLICE_AREA(slice_lines_area, "slice-lines")
{
    using namespace slice_lines;
    static const uint8_t sinkBytes[]={0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x55,0x49,0x89,0xfd,0x41,0x54,
        0x49,0x89,0xd4};
    SliceRegisterHook({"slice-lines","script immediate command sink",-1,0xa2d650,sinkBytes,sizeof(sinkBytes),15,
        reinterpret_cast<void*>(DirectSink),&g_directSinkTrampoline,true});
    SliceOnAdd("slice-lines",ReplayAdd,nullptr);
    for (uintptr_t rva : {kSet, kCreate, kUpdate, kDelete, kColor, kName})
        SliceOnFactory({"slice-lines", rva, OnFactory, nullptr, nullptr, true, 0});
}
