// Build 35924 layouts and wire conversion: SLICE_TERRAIN_ASSETS.md.
#include "slice_terrain_assets.h"
#include "slice_terrain_assets_checks.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace slice_terrain_assets {
namespace {
template<class T> void Store(void* dest, size_t offset, const T& value)
{
    std::memcpy(static_cast<uint8_t*>(dest) + offset, &value, sizeof(value));
}
void Vector(void* dest, size_t offset, void* data, size_t bytes)
{
    uintptr_t triple[] = {uintptr_t(data), data ? uintptr_t(data) + bytes : 0,
                         data ? uintptr_t(data) + bytes : 0};
    std::memcpy(static_cast<uint8_t*>(dest) + offset, triple, sizeof(triple));
}
bool EmptyVectors(uintptr_t proposal, bool constructions)
{
    const size_t offsets[] = {0, 0x18, 0x30, 0x48, 0xd0, 0xe8, 0x220, 0x270, 0x2f0};
    for (size_t offset : offsets) {
        SliceVec v;
        if (!SliceReadStdVector(proposal + offset, 1, 0, &v)) return false;
    }
    if (constructions) {
        SliceVec v;
        for (size_t offset : {size_t(0x288), size_t(0x2a0)})
            if (!SliceReadStdVector(proposal + offset, 1, 0, &v)) return false;
    }
    // Also refuse supplementary edits; dropping them would make capture lossy.
    for (size_t offset : {size_t(0x78), size_t(0xb0), size_t(0x250), size_t(0x2d0), size_t(0x330),
                          size_t(0x128), size_t(0x158), size_t(0x188), size_t(0x1b8), size_t(0x1e8), size_t(0x218)}) {
        size_t count;
        if (!SliceReadT(proposal + offset, &count) || count) return false;
    }
    return true;
}
bool ReadGrid(uintptr_t at, size_t stride, Grid* out)
{
    if (!SliceRead(at, out->rect.data(), 16)) return false;
    if (out->rect[2] < 0 || out->rect[3] < 0) return false;
    uint64_t cells = uint64_t(out->rect[2]) * uint64_t(out->rect[3]);
    if (cells > MaxBytes / stride) return false;
    SliceVec v;
    if (!SliceReadStdVector(at + 0x10, stride, MaxBytes / stride, &v) || v.count != cells) return false;
    out->data.resize(v.count * stride);
    return out->data.empty() || SliceRead(v.begin, out->data.data(), out->data.size());
}
bool Zero(uintptr_t at, size_t size)
{
    uint8_t bytes[40];
    if (size > sizeof(bytes) || !SliceRead(at, bytes, size)) return false;
    for (size_t i = 0; i < size; ++i) if (bytes[i]) return false;
    return true;
}

// Game allocations are tracked until a complete replay is ready. Partial build
// failures free every block without invoking the game's allocating C++ helpers.
struct Blocks {
    const GameMemory& memory;
    std::vector<void*> blocks;
    ~Blocks() { for (void* p : blocks) memory.release(p); }
    void* get(size_t n) {
        if (!n || n > MaxBytes) return nullptr;
        void* p = memory.allocate(n);
        if (!p) return nullptr;
        try { blocks.push_back(p); }
        catch (...) { memory.release(p); throw; }
        std::memset(p, 0, n);
        return p;
    }
    void transfer() { blocks.clear(); }
};

// The Proposal is the game-owned caller temporary, confined to this hook's
// thread. Validate writable mappings before its final memcpy; no permissions are
// changed, and no raw probe write is made to an unknown or read-only address.
bool Writable(uintptr_t address, size_t size)
{
    if (!address || address + size < address) return false;
    FILE* f = std::fopen("/proc/self/maps", "re");
    if (!f) return false;
    char line[512]; uintptr_t cursor = address;
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long begin, end; char mode[5]{};
        if (std::sscanf(line, "%lx-%lx %4s", &begin, &end, mode) != 3) continue;
        if (begin <= cursor && cursor < end) {
            if (mode[0] != 'r' || mode[1] != 'w') break;
            cursor = std::min(uintptr_t(end), address + size);
            if (cursor == address + size) break;
        }
    }
    std::fclose(f);
    return cursor == address + size;
}
bool PutString(uint8_t* target, const std::string& text, Blocks& blocks)
{
    char* chars;
    if (text.size() <= 15) chars = reinterpret_cast<char*>(target + 0x10);
    else {
        chars = static_cast<char*>(blocks.get(text.size() + 1));
        if (!chars) return false;
        Store(target, 0x10, text.size());
    }
    std::memcpy(chars, text.c_str(), text.size() + 1);
    Store(target, 0, chars); Store(target, 8, text.size());
    return true;
}
}

bool CarrierEmpty(uintptr_t proposal)
{
    if (!EmptyVectors(proposal, true)) return false;
    return Zero(proposal + 0x348, 24) && Zero(proposal + 0x370, 24) && Zero(proposal + 0x398, 40);
}
bool ReadTerrain(uintptr_t proposal, Terrain* out)
{
    *out = {};
    if (!EmptyVectors(proposal, true) || !ReadGrid(proposal + 0x338, 8, &out->height) ||
        !ReadGrid(proposal + 0x360, 1, &out->material) ||
        !SliceRead(proposal + 0x388, out->mask.rect.data(), 16)) return false;
    if (out->mask.rect[2] < 0 || out->mask.rect[3] < 0) return false;
    const uint64_t cells = uint64_t(out->mask.rect[2]) * uint64_t(out->mask.rect[3]);
    if (cells > MaxBytes * 8) return false;
    uintptr_t start, finish, cap; uint32_t beginBit, endBit;
    if (!SliceReadT(proposal + 0x398, &start) || !SliceReadT(proposal + 0x3a0, &beginBit) ||
        !SliceReadT(proposal + 0x3a8, &finish) || !SliceReadT(proposal + 0x3b0, &endBit) ||
        !SliceReadT(proposal + 0x3b8, &cap) || beginBit != 0 || endBit >= 64 ||
        finish < start || cap < finish || (finish - start) % 8 || (cap - start) % 8 ||
        finish - start > MaxBytes) return false;
    out->bits = (finish - start) * 8 + endBit;
    const size_t wordsBytes = ((out->bits + 63) / 64) * 8;
    if (out->bits != cells || wordsBytes > cap - start || wordsBytes > MaxBytes) return false;
    // x86-64 is little-endian: chopping the u64 storage into u32s is exactly the
    // TPTG conversion; EncodeTerrain clears the final unused bits.
    out->mask.data.resize(((cells + 31) / 32) * 4);
    if (!out->mask.data.empty() && (!SliceReadable(start, wordsBytes) ||
        !SliceRead(start, out->mask.data.data(), out->mask.data.size()))) return false;
    std::vector<uint8_t> check;
    return EncodeTerrain(*out, &check);
}
bool ReadAssets(uintptr_t proposal, Assets* out)
{
    *out = {};
    if (!EmptyVectors(proposal, false)) return false;
    Grid h, m;
    if (!ReadGrid(proposal + 0x338, 8, &h) || !ReadGrid(proposal + 0x360, 1, &m) ||
        !h.data.empty() || !m.data.empty()) return false;
    // An asset capture must not silently drop a terrain-mask edit.
    uintptr_t start, finish; uint32_t a, b;
    if (!SliceReadT(proposal + 0x398, &start) || !SliceReadT(proposal + 0x3a8, &finish) ||
        !SliceReadT(proposal + 0x3a0, &a) || !SliceReadT(proposal + 0x3b0, &b) || start != finish || a != b) return false;
    SliceVec add, remove;
    if (!SliceReadStdVector(proposal + 0x2a0, 0x8f0, UINT32_MAX, &add) ||
        !SliceReadStdVector(proposal + 0x288, 4, UINT32_MAX, &remove) || (!add.count && !remove.count)) return false;
    out->originalRemovals = uint32_t(remove.count);
    out->removals.resize(remove.count);
    if (remove.count && !SliceRead(remove.begin, out->removals.data(), remove.count * 4)) return false;
    for (int32_t id : out->removals) if (id <= 0) return false;
    size_t totalModels = 0;
    for (size_t i = 0; i < add.count; ++i) {
        const uintptr_t ce = add.begin + i * 0x8f0;
        int32_t type;
        if (!SliceReadT(ce + 0x20, &type) || type != 0xb) return false;
        SliceVec models;
        if (!SliceReadStdVector(ce + 0x478, 0x80, MaxAssetModels, &models) || !models.count ||
            (totalModels += models.count) > MaxTotalModels) return false;
        std::vector<Model> group(models.count);
        for (size_t j = 0; j < models.count; ++j) {
            const uintptr_t model = models.begin + j * 0x80;
            std::string path, extra;
            if (!SliceReadStdString(model, &path) || path.empty() ||
                !SliceReadStdString(model + 0x20, &extra) ||
                path.find('\0') != std::string::npos || extra.find('\0') != std::string::npos ||
                !SliceRead(model + 0x40, group[j].matrix.data(), 64)) return false;
            group[j].model = std::move(path); group[j].extra = std::move(extra);
        }
        out->groups.push_back(std::move(group));
    }
    std::vector<uint8_t> check;
    return EncodeAssets(*out, &check);
}
bool InstallTerrain(uintptr_t proposal, const Terrain& data, const GameMemory& memory)
{
    std::vector<uint8_t> valid;
    if (!memory.allocate || !memory.release || !EncodeTerrain(data, &valid) || !CarrierEmpty(proposal) ||
        !Writable(proposal + 0x338, 0x88)) return false;
    Blocks blocks{memory};
    uint8_t tail[0x88]{};
    const Grid* grids[] = {&data.height, &data.material, &data.mask};
    for (size_t i = 0; i < 3; ++i) {
        std::memcpy(tail + i * 0x28, grids[i]->rect.data(), 16);
        const size_t bytes = i == 2 ? ((data.bits + 63) / 64) * 8 : grids[i]->data.size();
        if (!bytes) continue;
        void* p = blocks.get(bytes);
        if (!p) return false;
        std::memcpy(p, grids[i]->data.data(), grids[i]->data.size());
        if (i < 2) Vector(tail, i * 0x28 + 0x10, p, bytes);
        else {
            // Canonicalize unused bits in the final game word as well.
            if (data.bits % 64) {
                uint64_t word; const size_t at = bytes - 8;
                std::memcpy(&word, static_cast<uint8_t*>(p) + at, 8);
                word &= (uint64_t(1) << (data.bits % 64)) - 1;
                Store(p, at, word);
            }
            Store(tail, 0x60, uintptr_t(p));
            Store(tail, 0x70, uintptr_t(p) + (data.bits / 64) * 8);
            Store(tail, 0x78, uint32_t(data.bits % 64));
            Store(tail, 0x80, uintptr_t(p) + bytes);
        }
    }
    std::memcpy(reinterpret_cast<void*>(proposal + 0x338), tail, sizeof(tail));
    blocks.transfer();
    return true;
}
bool InstallAssets(uintptr_t proposal, const Assets& data, const GameMemory& memory)
{
    std::vector<uint8_t> valid;
    if (!memory.allocate || !memory.release || !memory.constructEntity || !EncodeAssets(data, &valid) ||
        data.removals.size() > data.originalRemovals || !CarrierEmpty(proposal) ||
        !Zero(proposal + 0x288, 24) || !Zero(proposal + 0x2a0, 24) || !Writable(proposal + 0x288, 48)) return false;
    for (int32_t id : data.removals) if (id <= 0) return false;
    Blocks blocks{memory};
    const size_t ceBytes = data.groups.size() * 0x8f0;
    uint8_t* ces = ceBytes ? static_cast<uint8_t*>(blocks.get(ceBytes)) : nullptr;
    if (ceBytes && !ces) return false;
    for (size_t i = 0; i < data.groups.size(); ++i) {
        uint8_t* ce = ces + i * 0x8f0;
        // Fully verified straight-line ctor: 213 stores, no call/jump, one ret.
        // Construct at the final address so map sentinels and SSO stay valid.
        memory.constructEntity(ce);
        Store(ce, 0x20, int32_t(0xb)); Store(ce, 0x1fd, uint8_t(1));
        void* record = blocks.get(0x48);
        if (!record) return false;
        Store(record, 0x38, 0.75f); Store(record, 0x3c, 2.5f);
        Vector(ce, 0x578, record, 0x48);
        const auto& group = data.groups[i];
        uint8_t* models = static_cast<uint8_t*>(blocks.get(group.size() * 0x80));
        if (!models) return false;
        for (size_t j = 0; j < group.size(); ++j) {
            uint8_t* model = models + j * 0x80;
            if (!PutString(model, group[j].model, blocks) || !PutString(model + 0x20, group[j].extra, blocks)) return false;
            std::memcpy(model + 0x40, group[j].matrix.data(), 64);
        }
        Vector(ce, 0x478, models, group.size() * 0x80);
    }
    const size_t rmBytes = data.removals.size() * 4;
    void* ids = rmBytes ? blocks.get(rmBytes) : nullptr;
    if (rmBytes && !ids) return false;
    if (rmBytes) std::memcpy(ids, data.removals.data(), rmBytes);
    uint8_t vectors[48]{};
    Vector(vectors, 0, ids, rmBytes); Vector(vectors, 24, ces, ceBytes);
    std::memcpy(reinterpret_cast<void*>(proposal + 0x288), vectors, sizeof(vectors));
    blocks.transfer();
    return true;
}

namespace {
using NothrowNew = void* (*)(size_t, const std::nothrow_t&) noexcept;
NothrowNew gameNew = nullptr;
void (*gameDelete)(void*) = nullptr;
GameMemory gameMemory{};
void* Allocate(size_t size) { return gameNew(size, std::nothrow); }
bool ResolveMemory()
{
    void* throwingNew = nullptr; void* deleting = nullptr;
    if (!SliceReadT(SliceAddr(0x5a46cf0), &throwingNew) || !SliceReadT(SliceAddr(0x5a46ce8), &deleting)) return false;
    Dl_info owner{}, deleteOwner{};
    if (!dladdr(throwingNew, &owner) || !dladdr(deleting, &deleteOwner) ||
        owner.dli_fbase != deleteOwner.dli_fbase || !owner.dli_fname) return false;
    void* handle = dlopen(owner.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return false;
    void* noThrow = dlsym(handle, "_ZnwmRKSt9nothrow_t");
    Dl_info noThrowOwner{};
    const bool ok = dlsym(handle, "_Znwm") == throwingNew && dlsym(handle, "_ZdlPv") == deleting &&
                    noThrow && dladdr(noThrow, &noThrowOwner) && noThrowOwner.dli_fbase == owner.dli_fbase;
    if (ok) {
        gameNew = reinterpret_cast<NothrowNew>(noThrow);
        gameDelete = reinterpret_cast<void (*)(void*)>(deleting);
        gameMemory = {Allocate, gameDelete, reinterpret_cast<void (*)(void*)>(SliceAddr(0xdbec10))};
    }
    dlclose(handle); // the game retains its runtime for the process lifetime
    return ok;
}

struct HeldTool {
    uintptr_t tool = 0, vtable = 0;
    uint64_t since = 0, token = 0;
    int tid = 0;
    bool release = false, active = false;
};
std::mutex holdMutex;
HeldTool held[16];
uint64_t nextHoldToken = 0;
thread_local uint64_t preparedHold = 0;
void* destructorTrampoline = nullptr;
void ForgetTool(void* tool)
{
    std::lock_guard<std::mutex> lock(holdMutex);
    for (HeldTool& h : held) if (h.tool == uintptr_t(tool)) h = {};
}
void DestroyProposalAction(void* tool)
{
    // All three brush destructors enter this common base destructor before their
    // storage is released. The lock/cleanup frame ends before entering game code.
    ForgetTool(tool);
    reinterpret_cast<void (*)(void*)>(destructorTrampoline)(tool);
}
bool Flag(uintptr_t tool, uint8_t value)
{
    iovec local{&value, 1}, remote{reinterpret_cast<void*>(tool + 0xd0), 1};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == 1;
}
void PollHeld(const SliceAddCall* call, bool marker)
{
    std::lock_guard<std::mutex> lock(holdMutex);
    for (HeldTool& h : held) {
        if (!h.tool || !h.active) continue;
        if (marker) h.release = true;
        // Lua's calling thread is unproven; release only on the owning UI thread.
        if (!call || h.tid != call->tid || (!h.release && SliceNowMs() - h.since <= 4000)) continue;
        uintptr_t vt;
        if (SliceReadT(h.tool, &vt) && vt == h.vtable) Flag(h.tool, 0);
        h = {};
    }
}
void ObserveAdd(const SliceAddCall& call, void*) { PollHeld(&call, false); }
void TrackToolBeforeCallback(const SliceAddCall& call)
{
    // A vtable match alone cannot distinguish a live tool from freed/reused
    // storage. Retaining a pointer requires the installed destructor observer.
    preparedHold = 0;
    if (!destructorTrampoline || !SliceHookInstalled(0xe58980)) return;
    uintptr_t manager, invoker, tool, vt;
    if (!SliceStdFunctionParts(uintptr_t(call.done), &manager, &invoker) ||
        manager != SliceAddr(0xe57670) || invoker != SliceAddr(0xe593a0) ||
        !SliceReadT(uintptr_t(call.done), &tool) || !SliceReadT(tool, &vt)) return;
    if (vt != SliceAddr(0x5a09980) && vt != SliceAddr(0x5a0bcc0) && vt != SliceAddr(0x5a0bda0)) return;
    std::lock_guard<std::mutex> lock(holdMutex);
    HeldTool* target = nullptr;
    for (HeldTool& h : held) if (h.tool == tool) { target = &h; break; }
    if (!target) for (HeldTool& h : held) if (!h.tool) { target = &h; break; }
    if (!target) return;
    preparedHold = ++nextHoldToken;
    *target = {tool, vt, SliceNowMs(), preparedHold, call.tid, false, false};
}
void HoldAfterCallback()
{
    // Register BEFORE firing: a callback can destroy/recreate a tool at the same
    // address. Its destructor invalidates this token even if the vtable matches.
    std::lock_guard<std::mutex> lock(holdMutex);
    for (HeldTool& h : held) if (h.tool && h.token == preparedHold) {
        uintptr_t vt;
        if (SliceReadT(h.tool, &vt) && vt == h.vtable && Flag(h.tool, 1)) {
            h.active = true;
            SliceLog("[terrain-assets] held (effective for terraform and asset erase)\n");
        } else h = {};
        break;
    }
    preparedHold = 0;
}

thread_local SliceRecord pending{};
bool Prepare(const SliceAddCall& call, void*)
{
    if (SliceInjectWrite(pending, SliceArmedLine::One) == SliceInjectResult::NotWritten) return false;
    TrackToolBeforeCallback(call);
    return true;
}
void Landed(const SliceAddCall* call, SliceOutcome outcome, void*)
{
    const bool cancelled = outcome == SliceOutcome::CancelledFired || outcome == SliceOutcome::CancelledNotFired;
    if (cancelled && call && outcome == SliceOutcome::CancelledFired) HoldAfterCallback();
    if (outcome == SliceOutcome::RanNatively)
        SliceLog("[terrain-assets] strict capture cancel did not complete\n");
    SliceRecordFree(&pending);
}
// Return 0 when absent, 1 when read, -1 on an unreadable or oversize file.
int ReadFile(const char* prefix, const char* suffix, std::string* data, std::string* path)
{
    char instance[8];
    if (!SliceInstance(instance, sizeof(instance))) return 0;
    *path = std::string(SliceDataDir()) + prefix + instance + suffix;
    int fd = open(path->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1) {
        close(fd); return -1;
    }
    try { data->resize(st.st_size); } catch (...) { close(fd); throw; }
    size_t n = 0;
    while (n < data->size()) {
        ssize_t got = read(fd, data->data() + n, data->size() - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { close(fd); return -1; }
        n += size_t(got);
    }
    close(fd);
    return 1;
}
void Replay(const SliceFactoryCall& c)
{
    // .22 Lua carriers have no acknowledgement/retry protocol. Consume only
    // the intended empty proposal, exactly once even when decoding fails.
    if (!CarrierEmpty(c.rdx)) return;
    std::string text, path;
    int status = ReadFile("terrain_inject_", ".bin", &text, &path);
    if (status) {
        if (status > 0) unlink(path.c_str());
        Terrain terrain; std::vector<uint8_t> raw;
        const bool ok = status > 0 && Unbase64(text, &raw) && DecodeTerrain(raw, &terrain) &&
                        InstallTerrain(c.rdx, terrain, gameMemory);
        SliceLog("[terrain-assets] terrain carrier %s\n", ok ? "filled" : "rejected");
        return;
    }
    status = ReadFile("asset_inject_", ".txt", &text, &path);
    if (status) {
        if (status > 0) unlink(path.c_str());
        Assets assets;
        const bool ok = status > 0 && ParseAssetsFile(text, &assets) && InstallAssets(c.rdx, assets, gameMemory);
        SliceLog("[terrain-assets] asset carrier %s\n", ok ? "filled" : "rejected");
        return;
    }
    if (CarrierEmpty(c.rdx)) PollHeld(nullptr, true);
}
void Capture(const SliceFactoryCall& c)
{
    if (!SliceSessionLive()) return;
    std::vector<uint8_t> raw;
    SliceRecord rec{};
    try {
        Terrain terrain;
        if (ReadTerrain(c.rdx, &terrain)) {
            if (!EncodeTerrain(terrain, &raw)) return;
            std::string encoded = Base64(raw);
            SliceRecordPrintf(&rec, "TERRAINCAP %zu ", raw.size());
            SliceRecordAppend(&rec, encoded.data(), encoded.size()); SliceRecordAppend(&rec, "\n", 1);
        } else {
            Assets assets;
            if (!ReadAssets(c.rdx, &assets) || !EncodeAssets(assets, &raw)) {
                SliceLog("[terrain-assets] unsupported/mixed ProposalAction; strict action awaits the core block\n"); return;
            }
            std::string encoded = Base64(raw);
            SliceRecordPrintf(&rec, "ASSETCAP %zu ", raw.size());
            SliceRecordAppend(&rec, encoded.data(), encoded.size());
            SliceRecordPrintf(&rec, " %zu ", assets.removals.size());
            if (assets.removals.empty()) SliceRecordAppend(&rec, "-", 1);
            for (size_t i = 0; i < assets.removals.size(); ++i)
                SliceRecordPrintf(&rec, "%s%d", i ? "," : "", assets.removals[i]);
            SliceRecordAppend(&rec, "\n", 1);
        }
    } catch (...) { SliceRecordFree(&rec); throw; }
    if (rec.failed) { SliceRecordFree(&rec); return; }
    SliceRecordFree(&pending); pending = rec;
    if (!SliceArmCancel(c, {"terrain/assets", SliceDone::Required, false, nullptr, Landed, nullptr, Prepare})) {
        SliceRecordFree(&pending);
        SliceLog("[terrain-assets] strict replay could not arm; action awaits the core block\n");
    }
}
void OnProposal(const SliceFactoryCall& c, void*)
{
    try {
        if (c.retRva == 0x1971333) Replay(c);
        else if (c.retRva == 0xe59622) Capture(c);
    } catch (...) {
        // Only OUR STL allocations can throw. The game allocations use its
        // noexcept runtime wrapper, and its CE ctor contains no calls/branches.
        SliceLog("[terrain-assets] local allocation failure; strict action awaits the core block\n");
    }
}
}
}

slice_terrain_assets::GameMemory slice_terrain_assets::RuntimeMemory()
{
    return gameMemory;
}

void SliceTerrainPollHeldTools()
{
    SliceAddCall c{}; c.tid = SliceTid();
    slice_terrain_assets::PollHeld(&c, false);
}

SLICE_AREA(slice_terrain_assets, "slice-terrain-assets")
{
    using namespace slice_terrain_assets;
    for (const auto& p : kProbes) {
        uint8_t actual[16];
        if (!SliceRead(SliceAddr(p.rva), actual, p.size) || std::memcmp(actual, p.bytes, p.size)) {
            SliceLog("[terrain-assets] byte gate mismatch at %lx; capture/replay disabled\n", (unsigned long)p.rva); return;
        }
    }
    uint8_t ctor[2233];
    if (!SliceRead(SliceAddr(0xdbec10), ctor, sizeof(ctor))) return;
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t b : ctor) { hash ^= b; hash *= 1099511628211ull; }
    if (hash != 0xf1af220929f4f9d0ull || !ResolveMemory()) {
        SliceLog("[terrain-assets] nonthrowing ctor/runtime allocator identity unavailable; capture/replay disabled\n"); return;
    }
    SliceOnFactory({"slice-terrain-assets", 0x15ee930, OnProposal, nullptr, nullptr, true, 10});
    SliceOnAdd("slice-terrain-assets", ObserveAdd, nullptr);
    // A 5-byte entry patch stops before the RIP-relative LEA at +5. The three
    // derived destructors tail-call this base destructor (implementation notes).
    static constexpr uint8_t dtor[] = {0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x8d,0x05,0xb4,0x20,0xbb,0x04,0x48,0x89,0xe5};
    static constexpr Probe dtorCalls[] = {
        {0xdb9929, {0xe9,0x52,0xf0,0x09,0x00}, 5},
        {0xeb05ac, {0xe9,0xcf,0x83,0xfa,0xff}, 5},
        {0xebc856, {0xe9,0x25,0xc1,0xf9,0xff}, 5},
    };
    bool lifetime = true;
    for (const auto& p : dtorCalls) {
        uint8_t actual[16];
        if (!SliceRead(SliceAddr(p.rva), actual, p.size) || std::memcmp(actual, p.bytes, p.size)) lifetime = false;
    }
    if (lifetime) SliceRegisterHook({"slice-terrain-assets", "ProposalAction::~ProposalAction", -1, 0xe58980,
        dtor, sizeof(dtor), 5, reinterpret_cast<void*>(DestroyProposalAction), &destructorTrampoline, true});
    SliceLog("[terrain-assets] TPTG/TPAS capture/replay enabled with game runtime nothrow allocations\n");
}
