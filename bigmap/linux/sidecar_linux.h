// The terrain sidecar on the native Linux build 35924 (2026-09-28): write
// "<save>.terr" beside every save, and on the load of that same save restore
// every tile from it and skip the alignment pass. Same file format as Windows
// (../src/terrain_sidecar.h), so a sidecar written on one serves on the other.
//
// Simpler than the Windows path on purpose: the sidecar is used only when it
// holds EVERY tile of the terrain. Then no publication of the pass is needed at
// all and the pass is not called; its one other effect, each tile record's
// minZ/maxZ/version (publication 0xcf56d0: 0xcf58ad/0xcf58b4/0xcf58bb, scale
// at CTerrain+0x34, 0xcf5809), is written from the range noted while serving. A
// partial sidecar is applied before the pass and then overwritten by the stock pass
// -- the same result as without it. There is no per-copy skip to get wrong.
//
// Sites (docs/linux/PORT.md, "Terrain sidecar"):
//   SaveGame 0xc7ec00  (meta, shot, cfg, res, state, gui, SaveGameId* on the
//                       stack, bool, monitor); std::optional<SaveGameError> is
//                       returned in rax:rdx. The id's path must be empty (a
//                       non-empty one asserts, 0xc7ec5c); its name is at +0x20.
//   LoadGame 0xc7ca40  (hidden return, ctx, modRep, SaveGameId* in rcx, ...).
//   AddTile  0xcf71d0  (CTerrain*, int entity): grid at +0x18, 40-byte records,
//                       index (x-x0)+(y-y0)*nx (0xcf73f2).
// The save folder is <local>/save, <local> being TPF2MP_USERDATA or the folder
// of the game's own open crash_dump/stdout.txt (the menu's rule, SAVE-02).
#pragma once
#include "../src/terrain_sidecar.h"
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace linux_sidecar {
using LogFn = void (*)(const char*, ...);
// Publish only after all hooks are installed. Partial installs only forward.
inline std::atomic<bool>& Enabled() { static std::atomic<bool> on{false}; return on; }

struct LStr { const char* p; size_t n; char buf[16]; };   // libstdc++ std::string
struct SaveGameId { LStr path; LStr name; LStr ns; };
static_assert(sizeof(SaveGameId) == 0x60, "platform::SaveGameId");

// ---- the save folder ----------------------------------------------------------
inline std::string LocalFromOpenLog() {
    const char* suffix = "/crash_dump/stdout.txt";
    for (int fd = 0; fd < 4096; ++fd) {
        char link[64], target[1024];
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        const ssize_t n = readlink(link, target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = 0;
        const size_t len = size_t(n), sl = strlen(suffix);
        if (len > sl && !strcmp(target + len - sl, suffix)) return std::string(target, len - sl);
    }
    return std::string();
}
inline std::string SaveDir() {
    static std::mutex m; static std::string cached;
    std::lock_guard<std::mutex> lock(m);
    if (!cached.empty()) return cached;
    std::string local;
    const char* env = getenv("TPF2MP_USERDATA");
    struct stat st{};
    if (env && env[0] == '/' && !stat(env, &st) && S_ISDIR(st.st_mode)) local = env;
    else local = LocalFromOpenLog();
    while (local.size() > 1 && local.back() == '/') local.pop_back();
    if (!local.empty()) cached = local + "/save";
    return cached;
}
// <save folder>/<name>.sav for a SaveGameId, or empty.
inline std::string SavPath(const SaveGameId* id) {
    if (!id || id->path.n || !id->name.p || !id->name.n || id->name.n > 400) return std::string();
    const std::string name(id->name.p, id->name.n);
    if (name.find('/') != std::string::npos) return std::string();
    const std::string dir = SaveDir();
    return dir.empty() ? std::string() : dir + "/" + name + ".sav";
}
inline bool Mtime(const std::string& p, struct timespec* out) {
    struct stat st{};
    if (stat(p.c_str(), &st)) return false;
    *out = st.st_mtim; return true;
}

// ---- the load: served tiles and each tile's height range ---------------------
// A load builds two CTerrain versions (Windows served 68,086 tiles of a
// 36,992-tile map), each with its own grid, so the ranges are kept per grid.
struct Served {
    std::vector<void*> finished;   // CTerrains whose load pass was skipped
    // finished: this version's pass was skipped once; a later big pass on it must run.
    struct Grid { uint8_t* base = nullptr; std::vector<uint32_t> range; uint32_t applied = 0; bool finished = false; };
    std::mutex m;
    Grid grids[4];
    static constexpr uint32_t kNone = 0x0000FFFFu;   // lo | hi << 16 with lo > hi: never a real range
    void Reset() { std::lock_guard<std::mutex> l(m); for (auto& g : grids) g = Grid{}; finished.clear(); }
    // Caller holds m. The slot for `base` (n records), made if new; nullptr when all four are taken.
    Grid* Slot(uint8_t* base, uint32_t n, bool make) {
        for (auto& g : grids) if (g.base == base && g.range.size() == n) return &g;
        if (!make) return nullptr;
        for (auto& g : grids) if (!g.base) { g.base = base; g.range.assign(n, kNone); g.applied = 0; g.finished = false; return &g; }
        return nullptr;
    }
};
inline Served& S() { static Served* s = new Served; return *s; }

inline uint32_t HeightRange(const uint16_t* h) {
    unsigned lo = h[0], hi = h[0];
    for (size_t i = 1; i < TerrainSidecar::Samples; ++i) { const unsigned v = h[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    return lo | (hi << 16);
}
inline void Note(const TerrainSidecar::Grid& g, uint32_t idx, const TerrainSidecar::TileVector* v) {
    const uint32_t n = uint32_t(g.nx()) * uint32_t(g.ny());
    const uint32_t r = HeightRange(v->first);
    Served& s = S(); std::lock_guard<std::mutex> l(s.m);
    Served::Grid* slot = s.Slot(g.base, n, true);
    if (slot && idx < n && slot->range[idx] == Served::kNone) { slot->range[idx] = r; ++slot->applied; }
}
// True -- every record's minZ/maxZ written and version bumped as publication
// does -- when every record of `terrain` holds a full tile the sidecar applied.
// False, changing nothing, otherwise. Only while the sidecar is loaded (the
// load's own pass).
inline bool AllServedFinish(void* terrain) {
    if (!terrain || !TerrainSidecar::Loaded()) return false;
    const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
    if (!g.base || !g.records() || g.nx() <= 0 || g.ny() <= 0) return false;
    const uint32_t n = uint32_t(g.nx()) * uint32_t(g.ny());
    const float scale = *reinterpret_cast<const float*>(static_cast<uint8_t*>(terrain) + 0x34);
    if (!(scale >= 0.0f)) return false;
    Served& s = S(); std::lock_guard<std::mutex> l(s.m);
    Served::Grid* slot = s.Slot(g.base, n, false);
    if (!slot || slot->finished || slot->applied != n) return false;
    for (uint32_t i = 0; i < n; ++i)
        if (!TerrainSidecar::Eligible(TerrainSidecar::VectorOf(g.record(i))) || slot->range[i] == Served::kNone) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* r = g.record(i);
        *reinterpret_cast<float*>(r + 0x18) = float(slot->range[i] & 0xFFFF) * scale;
        *reinterpret_cast<float*>(r + 0x1c) = float(slot->range[i] >> 16) * scale;
        *reinterpret_cast<uint32_t*>(r + 0x20) += 1;
    }
    slot->finished = true;
    s.finished.push_back(terrain);
    return true;
}
// A served version whose pass has not come yet: the load builds two and passes
// each, so the first skip must not release the file (Windows 2026-09-27: the
// second version's last 4,859 AddTiles went unserved and its pass ran).
// With decoding at the pass (ServeAtPass), a version AddTile populated has no
// slot until its own pass: a terrain seen at AddTile and not finished counts.
inline bool SeenUnfinished();
inline bool VersionPending() {
    {
        Served& s = S(); std::lock_guard<std::mutex> l(s.m);
        for (const auto& g : s.grids) if (g.base && !g.finished) return true;
    }
    return SeenUnfinished();
}

// ---- AddTile: begin the armed sidecar, apply this tile ------------------------
using AddTileFn = void (*)(void*, int, uint64_t, uint64_t, uint64_t, uint64_t);
inline AddTileFn& OriginalAddTile() { static AddTileFn f = nullptr; return f; }

// ---- which CTerrain is live -----------------------------------------------------
// A load builds two CTerrain versions and frees one; the last one AddTile saw may
// be the freed one (Windows, 2026-09-21: "the last seen CTerrain is not this
// world's"). Every distinct CTerrain seen is kept (the last four), and the save
// captures the one that reads as a whole grid with the most full tiles. Reads go
// through process_vm_readv on ourselves: unmapped memory (a freed grid's records
// are one large, unmapped-on-free allocation) is an error, never a fault.
inline bool SafeRead(const void* src, void* dst, size_t n) {
    struct iovec l{dst, n}, r{const_cast<void*>(src), n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == ssize_t(n);
}
struct Seen { std::mutex m; void* t[4] = {}; };
inline Seen& SeenTerrains() { static Seen* s = new Seen; return *s; }
inline void NoteTerrain(void* terrain) {
    Seen& s = SeenTerrains();
    std::lock_guard<std::mutex> l(s.m);
    if (s.t[0] == terrain) return;
    int at = 3;
    for (int i = 0; i < 4; ++i) if (s.t[i] == terrain) at = i;
    for (int i = at; i > 0; --i) s.t[i] = s.t[i - 1];
    s.t[0] = terrain;
}
inline void ForgetTerrains() { Seen& s = SeenTerrains(); std::lock_guard<std::mutex> l(s.m); for (auto& t : s.t) t = nullptr; }
inline bool& ServeAtPass();
inline bool SeenUnfinished() {
    if (!ServeAtPass()) return false;
    void* seen[4];
    { Seen& s = SeenTerrains(); std::lock_guard<std::mutex> l(s.m); memcpy(seen, s.t, sizeof seen); }
    Served& s = S(); std::lock_guard<std::mutex> l(s.m);
    for (void* t : seen)
        if (t && std::find(s.finished.begin(), s.finished.end(), t) == s.finished.end()) return true;
    return false;
}
// The full tiles of `terrain`, read without trusting it; negative if it is not a readable grid.
inline long FullTiles(void* terrain) {
    uint8_t* grid = nullptr; uint8_t head[0x18];
    if (!terrain || !SafeRead(static_cast<uint8_t*>(terrain) + 0x18, &grid, sizeof grid) || !grid || !SafeRead(grid, head, sizeof head)) return -1;
    int32_t nx, ny; uint8_t* records;
    memcpy(&nx, head + 8, 4); memcpy(&ny, head + 0xc, 4); memcpy(&records, head + 0x10, 8);
    if (nx <= 0 || ny <= 0 || nx > 4096 || ny > 4096 || !records) return -2;
    const size_t n = size_t(nx) * size_t(ny);
    std::vector<uint8_t> rec(n * 40);
    if (!SafeRead(records, rec.data(), rec.size())) return -3;
    long full = 0;
    constexpr size_t Batch = 512;                    // under IOV_MAX
    std::vector<TerrainSidecar::TileVector> vecs(Batch);
    std::vector<struct iovec> local(Batch), remote(Batch);
    for (size_t at = 0; at < n; at += Batch) {
        size_t k = 0;
        for (size_t i = at; i < n && i < at + Batch; ++i) {
            void* v; memcpy(&v, rec.data() + i * 40 + 8, 8);
            if (!v) continue;
            local[k] = {&vecs[k], sizeof(TerrainSidecar::TileVector)};
            remote[k] = {v, sizeof(TerrainSidecar::TileVector)};
            ++k;
        }
        for (size_t i = 0; i < k; ++i) {                // one call per vector: a bad one fails alone
            if (process_vm_readv(getpid(), &local[i], 1, &remote[i], 1, 0) != ssize_t(sizeof(TerrainSidecar::TileVector))) continue;
            if (TerrainSidecar::Eligible(&vecs[i])) ++full;
        }
    }
    return full;
}
// The best readable CTerrain candidate, or nullptr; readability does not prove ownership. `why` lists each candidate's count.
inline void* PickTerrain(char* why, size_t cap) {
    void* cands[4];
    { Seen& s = SeenTerrains(); std::lock_guard<std::mutex> l(s.m); memcpy(cands, s.t, sizeof cands); }
    void* best = nullptr; long bestFull = 0; size_t w = 0;
    if (cap) why[0] = 0;
    for (void* t : cands) {
        if (!t) continue;
        const long full = FullTiles(t);
        if (w + 40 < cap) w += size_t(snprintf(why + w, cap - w, "%s%p=%ld", w ? " " : "", t, full));
        if (full > bestFull) { best = t; bestFull = full; }
    }
    return best;
}
// Search outward from this thread's last hit: ascending and descending loads
// both stay nearby, whereas a forward-only scan wraps the grid on every descent.
inline std::atomic<uint64_t>& CursorGeneration() { static std::atomic<uint64_t> gen{0}; return gen; }
struct LookupStats { std::atomic<uint64_t> calls{0}, probes{0}; };
inline LookupStats& Lookups() { static LookupStats s; return s; }
// Count locally; production accounting adds once per lookup, never per probe.
inline long FindRecord(const TerrainSidecar::Grid& g, int entity, uint32_t* probes = nullptr) {
    struct Cursor { const uint8_t* grid; uint32_t last; uint64_t generation; };
    static thread_local Cursor c = {nullptr, 0, 0};
    if (probes) *probes = 0;
    const uint32_t n = g.nx() > 0 && g.ny() > 0 ? uint32_t(g.nx()) * uint32_t(g.ny()) : 0;
    if (!n) return -1;
    const uint64_t gen = CursorGeneration().load(std::memory_order_relaxed);
    if (c.grid != g.base || c.generation != gen) { c.grid = g.base; c.last = n - 1; c.generation = gen; }
    auto is = [&](uint32_t i) {
        if (probes) ++*probes;
        const uint8_t* r = g.record(i);
        return *reinterpret_cast<const int32_t*>(r) == entity && *reinterpret_cast<uint8_t* const*>(r + 8);
    };
    const uint32_t last = c.last % n;
    // Each non-center record exactly once, including the opposite point of an
    // even ring. Checking the center last also handles a one-record grid.
    for (uint32_t d = 1; d <= n / 2; ++d) {
        const uint32_t up = last + d < n ? last + d : last + d - n;
        const uint32_t down = last >= d ? last - d : last + n - d;
        if (is(up)) { c.last = up; return long(up); }
        if (down != up && is(down)) { c.last = down; return long(down); }
    }
    return is(last) ? long(last) : -1;
}
// terrain_sidecar_decode_at_pass: AddTile only notes the terrain; every tile is
// decoded in parallel at the pass (CatchUp), off the load's own thread (see
// terrain_serve.h g_serveAtPass).
inline bool& ServeAtPass() { static bool on = false; return on; }
inline void AddTileHook(void* terrain, int entity, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    OriginalAddTile()(terrain, entity, a, b, c, d);
    if (!Enabled().load() || !terrain) return;
    NoteTerrain(terrain);
    static std::mutex beginLock;
    if (TerrainSidecar::g_pending) {
        std::lock_guard<std::mutex> l(beginLock);
        if (TerrainSidecar::g_pending) { S().Reset(); TerrainSidecar::BeginIfPending(terrain, nullptr); }
    }
    if (!TerrainSidecar::Loaded() && !TerrainSidecar::TryStream(terrain)) return;
    if (ServeAtPass()) return;
    const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
    if (!g.base || !g.records()) return;
    uint32_t probes = 0;
    const long idx = FindRecord(g, entity, &probes);
    Lookups().calls.fetch_add(1, std::memory_order_relaxed);
    Lookups().probes.fetch_add(probes, std::memory_order_relaxed);
    if (idx < 0) return;
    if (!TerrainSidecar::Has(uint32_t(idx))) {
        TerrainSidecar::RefreshIfDue();              // a stream: maybe it has arrived since
        if (!TerrainSidecar::Has(uint32_t(idx))) return;
    }
    static thread_local BlockCodec::DecodeScratch* scratch = nullptr;
    if (!scratch) scratch = new (std::nothrow) BlockCodec::DecodeScratch;
    if (!scratch || !TerrainSidecar::ApplyTile(g, uint32_t(idx), *scratch)) return;
    Note(g, uint32_t(idx), TerrainSidecar::VectorOf(g.record(uint32_t(idx))));
}

// ---- SaveGame: capture at entry, stamp and rename after ----------------------
struct Ret16 { uint64_t a, b; };
using SaveFn = Ret16 (*)(void*, void*, void*, void*, void*, void*, const SaveGameId*, uint64_t, void*);
inline SaveFn& OriginalSave() { static SaveFn f = nullptr; return f; }
inline LogFn& Log() { static LogFn f = nullptr; return f; }
inline bool& WriteOn() { static bool on = true; return on; }

// Delete every <x>.terr / <x>.terr.tmp in `dir` whose <x>.sav is gone. Returns the count.
inline int SweepOrphans(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    int removed = 0;
    while (struct dirent* e = readdir(d)) {
        std::string f = e->d_name;
        std::string stem;
        if (f.size() > 9 && f.compare(f.size() - 9, 9, ".terr.tmp") == 0) stem = f.substr(0, f.size() - 9);
        else if (f.size() > 5 && f.compare(f.size() - 5, 5, ".terr") == 0) stem = f.substr(0, f.size() - 5);
        else continue;
        struct stat st{};
        if (stat((dir + "/" + stem + ".sav").c_str(), &st) != 0 && !remove((dir + "/" + f).c_str())) ++removed;
    }
    closedir(d);
    return removed;
}
// The .sav in `dir` written since `since` (an autosave's own name), newest first.
inline std::string WrittenSince(const std::string& dir, const struct timespec& since) {
    DIR* d = opendir(dir.c_str());
    if (!d) return std::string();
    std::string best; struct timespec bestT{};
    while (struct dirent* e = readdir(d)) {
        const size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".sav")) continue;
        struct timespec t{};
        const std::string p = dir + "/" + e->d_name;
        if (!Mtime(p, &t)) continue;
        const bool after = t.tv_sec > since.tv_sec || (t.tv_sec == since.tv_sec && t.tv_nsec >= since.tv_nsec);
        const bool newer = t.tv_sec > bestT.tv_sec || (t.tv_sec == bestT.tv_sec && t.tv_nsec > bestT.tv_nsec);
        if (after && (best.empty() || newer)) { best = p; bestT = t; }
    }
    closedir(d);
    return best;
}
inline Ret16 SaveHook(void* meta, void* shot, void* cfg, void* res, void* state, void* gui, const SaveGameId* id, uint64_t flag, void* monitor) {
    if (!Enabled().load()) return OriginalSave()(meta, shot, cfg, res, state, gui, id, flag, monitor);
    struct timespec callStart{}; clock_gettime(CLOCK_REALTIME, &callStart);
    std::string sav = WriteOn() ? SavPath(id) : std::string(), tmp;
    long tiles = -1; uint64_t bytes = 0; long long ms = 0;
    struct timespec before{}; const bool hadBefore = !sav.empty() && Mtime(sav, &before);
    char picked[200] = {0};
    void* terrain = sav.empty() ? nullptr : PickTerrain(picked, sizeof picked);
    if (!sav.empty() && !terrain && Log()) Log()("terrain sidecar: no live terrain among those seen (%s); no sidecar for this save", picked);
    if (!sav.empty() && terrain) {
        const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
        if (g.base && g.records() && g.nx() > 0 && g.ny() > 0) {
            char terr[1040]; TerrainSidecar::SidecarPath(sav.c_str(), terr, sizeof terr);
            if (terr[0]) {
                tmp = std::string(terr) + ".tmp";
                auto* enc = new (std::nothrow) BlockCodec::EncodeScratch;
                const auto t0 = std::chrono::steady_clock::now();
                if (enc) tiles = TerrainSidecar::Write(g, 0, tmp.c_str(), enc, &bytes);
                ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                delete enc;
            }
        }
    }
    const Ret16 r = OriginalSave()(meta, shot, cfg, res, state, gui, id, flag, monitor);
    if (tiles <= 0) { if (!tmp.empty()) remove(tmp.c_str()); return r; }
    struct timespec after{};
    bool fresh = Mtime(sav, &after) && (!hadBefore || after.tv_sec != before.tv_sec || after.tv_nsec != before.tv_nsec);
    if (!fresh) {   // an autosave the engine wrote under another name
        const std::string dir = sav.substr(0, sav.rfind('/'));
        const std::string written = WrittenSince(dir, callStart);
        if (!written.empty()) { sav = written; fresh = true; }
    }
    char terr[1040]; TerrainSidecar::SidecarPath(sav.c_str(), terr, sizeof terr);
    const uint64_t fp = fresh && terr[0] ? TerrainSidecar::HashFile(sav.c_str()) : 0;
    const bool ok = fp && TerrainSidecar::Refingerprint(tmp.c_str(), fp) && !rename(tmp.c_str(), terr);
    if (!ok) {
        remove(tmp.c_str());
        if (Log()) Log()("terrain sidecar: the save was not written or could not be hashed; the captured sidecar is discarded");
        return r;
    }
    const int swept = SweepOrphans(sav.substr(0, sav.rfind('/')));
    if (Log()) Log()("terrain sidecar: wrote %ld tiles, %.1f MiB, capture %lld ms on %u threads, beside %s%s", tiles, double(bytes) / 1048576.0,
                     ms, TerrainSidecar::WriteThreads(TerrainSidecar::g_writeThreads), sav.c_str(), swept ? " (sidecars of deleted saves removed)" : "");
    return r;
}

// ---- LoadGame: arm the sidecar for this save ----------------------------------
using LoadFn = void* (*)(void*, void*, void*, const SaveGameId*, void*, void*, void*, void*, void*, void*, void*);
inline LoadFn& OriginalLoad() { static LoadFn f = nullptr; return f; }
inline void* LoadHook(void* ret, void* ctx, void* mods, const SaveGameId* id, void* a4, void* a5, void* s0, void* s1, void* s2, void* s3, void* s4) {
    if (!Enabled().load()) return OriginalLoad()(ret, ctx, mods, id, a4, a5, s0, s1, s2, s3, s4);
    // Worker threads can survive loads and the allocator can reuse grid addresses.
    CursorGeneration().fetch_add(1, std::memory_order_relaxed);
    Lookups().calls.store(0, std::memory_order_relaxed);
    Lookups().probes.store(0, std::memory_order_relaxed);
    ForgetTerrains();
    S().Reset();
    TerrainSidecar::EndApply();
    TerrainSidecar::g_pending = false;
    TerrainSidecar::g_streamWanted = false;
    const std::string sav = SavPath(id);
    if (!sav.empty()) {
        const auto t0 = std::chrono::steady_clock::now();
        TerrainSidecar::ArmForLoad(sav.c_str());
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (Log()) Log()("terrain sidecar: loading %s, fingerprint %016llx in %lld ms; %s", sav.c_str(),
                         (unsigned long long)TerrainSidecar::g_saveFingerprint, ms,
                         TerrainSidecar::g_pending && TerrainSidecar::FingerprintOfPath(TerrainSidecar::g_sidecarPath) == TerrainSidecar::g_saveFingerprint
                             ? TerrainSidecar::g_sidecarPath : TerrainSidecar::g_streamDir[0] ? "no sidecar beside it; a stream from the host may follow" : "no matching sidecar");
    } else if (Log()) Log()("terrain sidecar: this load's save path could not be resolved; loading stock");
    return OriginalLoad()(ret, ctx, mods, id, a4, a5, s0, s1, s2, s3, s4);
}

// ---- the alignment pass: skip it when the sidecar served every tile ----------
inline bool VersionFinished(void* terrain) {
    const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
    if (!g.base || g.nx() <= 0 || g.ny() <= 0) return false;
    Served& s = S(); std::lock_guard<std::mutex> l(s.m);
    const Served::Grid* slot = s.Slot(g.base, uint32_t(g.nx()) * uint32_t(g.ny()), false);
    return slot && slot->finished;
}
// Serve every tile the sidecar holds that AddTile did not (a stream that had not
// arrived yet), on several threads. Returns the tiles served here.
inline uint32_t CatchUp(void* terrain) {
    const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
    if (!g.base || !g.records() || g.nx() <= 0 || g.ny() <= 0) return 0;
    const uint32_t n = uint32_t(g.nx()) * uint32_t(g.ny());
    std::vector<uint32_t> todo;
    {
        Served& s = S(); std::lock_guard<std::mutex> l(s.m);
        const Served::Grid* slot = s.Slot(g.base, n, false);
        for (uint32_t i = 0; i < n; ++i)
            if ((!slot || slot->range[i] == Served::kNone) && TerrainSidecar::Has(i)) todo.push_back(i);
    }
    if (todo.empty()) return 0;
    std::atomic<size_t> next{0};
    std::atomic<uint32_t> served{0};
    auto work = [&] {
        auto* sc = new (std::nothrow) BlockCodec::DecodeScratch;
        if (!sc) return;
        for (size_t k; (k = next.fetch_add(1)) < todo.size();) {
            const uint32_t i = todo[k];
            if (!TerrainSidecar::ApplyTile(g, i, *sc)) continue;
            Note(g, i, TerrainSidecar::VectorOf(g.record(i)));
            served.fetch_add(1);
        }
        delete sc;
    };
    const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads && todo.size() > 64; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    return served.load();
}
// The pass is here and the sidecar is a stream still arriving: wait for it when
// that beats the pass, then serve what arrived. On Linux the pass computes every
// tile unless all are served (no per-tile publication skip), so the whole pass
// is what waiting saves.
constexpr double kPassMsPerTile = 1.0;
inline void SettleStream(void* terrain) {
    TerrainSidecar::TryStream(terrain, true);
    const TerrainSidecar::StreamState s = TerrainSidecar::Progress();
    if (!s.loaded) return;
    if (s.streaming) {
        const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
        const uint32_t n = g.base && g.nx() > 0 && g.ny() > 0 ? uint32_t(g.nx()) * uint32_t(g.ny()) : 0;
        char why[200] = {0};
        const bool complete = TerrainSidecar::WaitForStream(n, kPassMsPerTile, 5000, 300000, why, sizeof why);
        if (Log()) Log()("terrain stream: %s at the pass: %s", complete ? "complete" : "not waited for", why);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t caught = CatchUp(terrain);
    if (caught && Log()) Log()("terrain sidecar: %u tiles served at the pass in %lld ms", caught,
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}
// Called from the redirected UpdateSubterrains call. True when the pass must
// not run (the caller then returns without calling it).
inline bool SkipPass(void* self, size_t blocks) {
    if (!Enabled().load() || !self || blocks <= 512) return false;
    void* terrain = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 8);
    if (!terrain) return false;
    if (VersionFinished(terrain)) return false;      // it passed already: a pass in play, never the load's
    SettleStream(terrain);
    if (!AllServedFinish(terrain)) return false;
    if (Log()) Log()("alignment pass: %zu blocks skipped -- every tile was served from the sidecar (its min/max and version written from the served cache)", blocks);
    return true;
}
// After a load-sized pass: release the sidecar, unless it was skipped and
// another served version is still to pass.
inline void PassDone(size_t blocks, bool skipped) {
    if (blocks <= 512) return;
    if (skipped && VersionPending()) { if (Log()) Log()("terrain sidecar: pass skipped; kept for the load's other terrain version"); return; }
    // A stream first arriving after this load pass must never overwrite play edits.
    TerrainSidecar::g_streamWanted = false;
    const uint32_t applied = [] { Served& s = S(); std::lock_guard<std::mutex> l(s.m); uint32_t a = 0; for (auto& g : s.grids) a += g.applied; return a; }();
    const uint64_t calls = Lookups().calls.load(std::memory_order_relaxed);
    const uint64_t probes = Lookups().probes.load(std::memory_order_relaxed);
    if (TerrainSidecar::EndApply() && Log()) Log()("terrain sidecar: load done, %u tiles applied from the sidecar, %.1f record probes per AddTile lookup; file released",
        applied, calls ? double(probes) / double(calls) : 0.0);
}
}  // namespace linux_sidecar
