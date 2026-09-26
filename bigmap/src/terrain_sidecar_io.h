// Steam 35924: the SaveGame / LoadGame hooks of the terrain sidecar -- the half
// that WRITES "<save>.terr" and ARMS it for a load. terrain_sidecar.h is the file
// format, terrain_serve.h serves a loaded sidecar's tiles at CTerrain::AddTile.
// Until this header existed nothing wrote or armed a sidecar, and
// terrain_sidecar=1 was inert.
//
// SaveGame (0x2e97c0, game\serializer.cpp):
//   std::optional<platform::SaveGameError> SaveGame(const GameMetadata&, const
//   GameScreenshot&, const GameConfigData&, const GameRes&, const GameState&,
//   const GuiSaveData&, const platform::SaveGameId&, bool, IProgressMonitor&)
//   -- a hidden return pointer first, so the SaveGameId is the 8th argument.
// LoadGame (0x2e5ec0, sole caller 0x67d130):
//   std::unique_ptr<CGame> LoadGame(GameUserProfileContext, std::unique_ptr<const
//   ModRep>, const platform::SaveGameId&, ...) -- hidden return pointer, the two
//   by-value aggregates by address, so the SaveGameId is the 4th argument (r9).
//
// platform::SaveGameId { std::wstring path; std::string name; std::string ns }
// (0x60 bytes, MSVC SSO). The file is <dir>\<name>.sav, where <dir> is `path`
// when it is not empty and otherwise the backend's directory for `ns`, exactly
// as platform::StandardSaveGameBackend::GetSavegameInfo (0x2471830) builds it:
//   app     = 0xbb23c0()                      the application object
//   backend = 0xbb2db0(app)                   app->impl->vfunc[3](): ISaveGameBackend*
//   dir     = 0x2471640(backend, &id.ns)      std::string* (UTF-8), nullptr if unknown
// The backend is only asked when its vftable is StandardSaveGameBackend's
// (0x38e5758, the one concrete class in the build).
//
// WHEN THE SIDECAR IS WRITTEN. The world is frozen for the length of SaveGame
// (it serialises a const GameState&), so the terrain is captured at the detour's
// ENTRY, before the engine writes a byte: what the sidecar holds is what the save
// holds. The save's fingerprint does not exist yet, so the file goes out as
// "<save>.terr.tmp" with fingerprint 0; when the engine returns and the .sav's
// write time has moved, the file is stamped with the new .sav's hash and renamed
// over "<save>.terr". A failed save leaves no sidecar. Writing after the engine
// returned would race the first alignment of the resumed simulation.
//
// WHICH TERRAIN. g_alignmentTerrain, the CTerrain the alignment system last
// worked on (alignment_batch.h); cleared at every LoadGame so it is always this
// world's. It is only trusted when its grid is sane and every full tile's cache
// lies in the terrain pager's arena (a freed CTerrain's tiles do not).
//
// ORPHANS. Autosaves rotate: the engine deletes the oldest .sav and knows nothing
// of its sidecar. After each write, a .terr in that folder whose .sav is gone is
// deleted.
#pragma once
static int g_sidecarWrite = 1;            // terrain_sidecar_write
static int g_sidecarMaxTiles = 0;     // terrain_sidecar_max_tiles: grid records; 0 = no limit
namespace SidecarIo {
constexpr uintptr_t kSaveGameRva = 0x2e97c0, kLoadGameRva = 0x2e5ec0;
constexpr uintptr_t kAppRva = 0xbb23c0, kBackendRva = 0xbb2db0, kSaveDirRva = 0x2471640, kBackendVftableRva = 0x38e5758;
// push rbx..r15; sub rsp,0x8a0            (19 bytes, none RIP-relative)
static const uint8_t kSaveGameBytes[19] = {0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x08, 0x00, 0x00};
// push rbp,rbx,rsi,rdi,r12..r15; lea rbp,[rsp-0x498]   (21 bytes)
static const uint8_t kLoadGameBytes[21] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x68, 0xfb, 0xff, 0xff};
static const uint8_t kAppBytes[8] = {0x48, 0x8b, 0x05, 0xa1, 0x07, 0x78, 0x03, 0xc3};            // mov rax,[rip+..]; ret
static const uint8_t kBackendBytes[10] = {0x48, 0x8b, 0x09, 0x48, 0x8b, 0x01, 0x48, 0xff, 0x60, 0x18};   // mov rcx,[rcx]; mov rax,[rcx]; jmp [rax+0x18]
static const uint8_t kSaveDirBytes[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x89, 0x74, 0x24, 0x60, 0x4c, 0x8b, 0xca, 0x48, 0x89};

// MSVC std::string / std::wstring, as the engine lays them out.
struct GStr  { union { char buf[16]; char* ptr; }; size_t size, cap; const char* data() const { return cap > 15 ? ptr : buf; } };
struct GWStr { union { wchar_t buf[8]; wchar_t* ptr; }; size_t size, cap; const wchar_t* data() const { return cap > 7 ? ptr : buf; } };
struct SaveGameId { GWStr path; GStr name; GStr ns; };
static_assert(sizeof(GStr) == 0x20 && sizeof(GWStr) == 0x20 && sizeof(SaveGameId) == 0x60, "platform::SaveGameId layout");

using SaveGameFn = void* (__fastcall*)(void* ret, void* meta, void* shot, void* cfg, void* res, void* state, void* gui, const SaveGameId* id, bool flag, void* monitor);
using LoadGameFn = void* (__fastcall*)(void* ret, void* ctx, void* modRep, const SaveGameId* id, void* mods, void* settings, void* pairs, bool flag, void* s1, void* s2, void* monitor);
static SaveGameFn originalSave = nullptr;
static LoadGameFn originalLoad = nullptr;
static uintptr_t base = 0;
static bool backendOk = false;            // the three accessors verified byte for byte
static bool (*inArena)(const void*) = nullptr;
static uint64_t (*liveTiles)() = nullptr;   // the terrain pager's live tile count
static volatile LONG64 saves = 0, written = 0, skipped = 0, loadsArmed = 0;

// The save's directory for an id with an empty path, from the engine's backend.
// SEH only in this frame (no C++ objects): the id and the backend are the engine's.
static bool BackendDir(const SaveGameId* id, char* out, size_t cap) {
    if (!backendOk) return false;
    __try {
        void* app = reinterpret_cast<void* (*)()>(base + kAppRva)();
        if (!app) return false;
        void* backend = reinterpret_cast<void* (*)(void*)>(base + kBackendRva)(app);
        if (!backend || *reinterpret_cast<uintptr_t*>(backend) != base + kBackendVftableRva) return false;
        const GStr* dir = reinterpret_cast<const GStr* (*)(void*, const GStr*)>(base + kSaveDirRva)(backend, &id->ns);
        if (!dir || !dir->size || dir->size + 1 > cap) return false;
        memcpy(out, dir->data(), dir->size); out[dir->size] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// "<dir>\<name>.sav" as UTF-8. Pure for an id that carries its own path.
static bool ResolveSavePath(const SaveGameId* id, char* out, size_t cap) {
    if (!id || cap < 16) return false;
    __try {
        if (!id->name.size || id->name.size > 512 || id->path.size > 900 || id->ns.size > 256) return false;
        size_t n = 0;
        if (id->path.size) {
            int got = WideCharToMultiByte(CP_UTF8, 0, id->path.data(), int(id->path.size), out, int(cap) - 1, nullptr, nullptr);
            if (got <= 0) return false;
            n = size_t(got); out[n] = 0;
        } else {
            if (!BackendDir(id, out, cap)) return false;
            n = strlen(out);
        }
        if (n && out[n - 1] != '\\' && out[n - 1] != '/') { if (n + 1 >= cap) return false; out[n++] = '\\'; }
        if (n + id->name.size + 5 >= cap) return false;
        memcpy(out + n, id->name.data(), id->name.size); n += id->name.size;
        memcpy(out + n, ".sav", 5);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Is this CTerrain live and ours to read? Counts its full tiles; negative = do not trust it:
// -1 null, -2 no grid, -3 dimensions, -4 a full tile outside the pager's arena, -5 unreadable.
static long CheckTerrain(void* terrain, long* recordsOut) {
    if (!terrain) return -1;
    __try {
        TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
        if (!g.base || !g.records()) return -2;
        const int32_t nx = g.nx(), ny = g.ny();
        if (nx <= 0 || ny <= 0 || nx > 4096 || ny > 4096) return -3;
        const uint32_t count = uint32_t(nx) * uint32_t(ny);
        if (recordsOut) *recordsOut = long(count);
        long full = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const TerrainSidecar::TileVector* v = TerrainSidecar::VectorOf(g.record(i));
            if (!TerrainSidecar::Eligible(v)) continue;
            if (inArena && !inArena(v->first)) return -4;   // a tile outside the arena: not the pager's world
            ++full;
        }
        return full;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -5; }
}
// The live CTerrain among the candidates: the alignment system's last, and every CTerrain
// this load populated. A load holds two versions and frees one; the freed one fails the
// checks or counts fewer full tiles than the pager has live. The winner must hold at least
// 99% of the pager's live tiles, so a half-overwritten stale grid is never captured.
static void* PickTerrain(long* fullOut, long* recordsOut, char* why, size_t whyCap) {
    void* cands[5] = { g_alignmentTerrain, TerrainServe::seenTerrains[0], TerrainServe::seenTerrains[1], TerrainServe::seenTerrains[2], TerrainServe::seenTerrains[3] };
    void* best = nullptr; long bestFull = 0, bestRecords = 0; size_t w = 0;
    if (whyCap) why[0] = 0;
    for (int i = 0; i < 5; ++i) {
        if (!cands[i]) continue;
        bool dup = false; for (int k = 0; k < i; ++k) if (cands[k] == cands[i]) dup = true;
        if (dup) continue;
        long records = 0; const long full = CheckTerrain(cands[i], &records);
        if (w + 40 < whyCap) w += size_t(snprintf(why + w, whyCap - w, "%s%p=%ld", w ? " " : "", cands[i], full));
        if (full > bestFull) { best = cands[i]; bestFull = full; bestRecords = records; }
    }
    const uint64_t live = liveTiles ? liveTiles() : 0;
    if (w + 40 < whyCap) snprintf(why + w, whyCap - w, "; pager live=%llu", (unsigned long long)live);
    if (!best || (live && uint64_t(bestFull) * 100 < live * 99)) return nullptr;
    *fullOut = bestFull; *recordsOut = bestRecords;
    return best;
}
static long GuardedWrite(void* terrain, const char* path, BlockCodec::EncodeScratch* scratch, uint64_t* bytes) {
    __try { return TerrainSidecar::Write(TerrainSidecar::GridOf(terrain), 0, path, scratch, bytes); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}
static bool WriteTime(const char* utf8, FILETIME* out) {
    wchar_t w[1040]; WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!TerrainSidecar::WidePath(utf8, w, 1040) || !GetFileAttributesExW(w, GetFileExInfoStandard, &fa)) return false;
    *out = fa.ftLastWriteTime; return true;
}
// Delete every "<x>.terr" (and stale "<x>.terr.tmp") in the save's folder whose "<x>.sav" is gone.
static int SweepOrphans(const char* savPath) {
    wchar_t dir[1040];
    if (!TerrainSidecar::WidePath(savPath, dir, 1040)) return 0;
    wchar_t* slash = wcsrchr(dir, L'\\'); wchar_t* fwd = wcsrchr(dir, L'/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    if (!slash) return 0;
    slash[1] = 0;
    int removed = 0;
    for (const wchar_t* pat : { L"*.terr", L"*.terr.tmp" }) {
        wchar_t pattern[1040]; wcscpy_s(pattern, dir); wcscat_s(pattern, pat);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t stem[600]; wcscpy_s(stem, fd.cFileName);
            wchar_t* ext = wcsstr(stem, L".terr");
            if (!ext) continue;
            *ext = 0;
            wchar_t sav[1040], terr[1040];
            wcscpy_s(sav, dir); wcscat_s(sav, stem); wcscat_s(sav, L".sav");
            if (GetFileAttributesW(sav) != INVALID_FILE_ATTRIBUTES) continue;
            wcscpy_s(terr, dir); wcscat_s(terr, fd.cFileName);
            if (DeleteFileW(terr)) ++removed;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return removed;
}

struct PendingWrite { bool active; char sav[1040], tmp[1056], terr[1048]; long tiles; uint64_t bytes; FILETIME before; bool hadBefore; LONGLONG encodeMs; FILETIME callStart; };

// The .sav in the resolved save's folder that the engine wrote during this call
// (write time at or after callStart); false when there is none, or more than one.
// An autosave's file is not "<id.name>.sav" -- the engine names and rotates it
// (autosave_<game>_<date>.sav) -- so its sidecar was always discarded
// ("the save was not written"), and a hot join shares exactly such a save.
static bool WrittenDuringCall(const char* resolved, const FILETIME& callStart, char* out, size_t cap) {
    wchar_t dir[1040];
    if (!TerrainSidecar::WidePath(resolved, dir, 1040)) return false;
    wchar_t* slash = wcsrchr(dir, L'\\'); wchar_t* fwd = wcsrchr(dir, L'/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    if (!slash) return false;
    slash[1] = 0;
    wchar_t pattern[1040]; wcscpy_s(pattern, dir); wcscat_s(pattern, L"*.sav");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    // FAT/NTFS write-time granularity and clock steps: allow two seconds before the call
    ULARGE_INTEGER floor{}; floor.LowPart = callStart.dwLowDateTime; floor.HighPart = callStart.dwHighDateTime;
    floor.QuadPart -= 2ull * 10000000ull;
    // Exactly one: a sidecar stamped with ANOTHER save's hash would serve this
    // world's terrain when that save loads (e.g. the lobby copying mp_shared.sav
    // during the call). Two candidates = no sidecar.
    int found = 0; wchar_t bestName[600] = L"";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const size_t len = wcslen(fd.cFileName);
        if (len < 5 || _wcsicmp(fd.cFileName + len - 4, L".sav") != 0) continue;   // not *.sav.lua
        ULARGE_INTEGER t{}; t.LowPart = fd.ftLastWriteTime.dwLowDateTime; t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (t.QuadPart < floor.QuadPart) continue;
        ++found; wcscpy_s(bestName, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found != 1) return false;
    wchar_t full[1100]; wcscpy_s(full, dir); wcscat_s(full, bestName);
    const int got = WideCharToMultiByte(CP_UTF8, 0, full, -1, out, int(cap), nullptr, nullptr);
    return got > 0;
}

// Entry of SaveGame: capture the terrain into "<save>.terr.tmp".
static void BeforeSave(const SaveGameId* id, PendingWrite& p) {
    p = PendingWrite{};
    GetSystemTimeAsFileTime(&p.callStart);
    InterlockedIncrement64(&saves);
    if (!g_sidecarWrite || !g_terrainServe) return;
    if (!ResolveSavePath(id, p.sav, sizeof p.sav)) { InterlockedIncrement64(&skipped); if (H) H->log("terrain sidecar: this save's path could not be resolved; no sidecar"); return; }
    long records = 0, full = 0; char why[256];
    void* terrain = PickTerrain(&full, &records, why, sizeof why);
    if (!terrain) { InterlockedIncrement64(&skipped); if (H) H->log("terrain sidecar: no live terrain to capture (candidates: %s); no sidecar for this save", why[0] ? why : "none"); return; }
    if (g_sidecarMaxTiles > 0 && records > g_sidecarMaxTiles) {
        InterlockedIncrement64(&skipped);
        if (H) H->log("terrain sidecar: %ld grid records is over terrain_sidecar_max_tiles=%d; no sidecar for this save", records, g_sidecarMaxTiles);
        return;
    }
    TerrainSidecar::SidecarPath(p.sav, p.terr, sizeof p.terr);
    if (!p.terr[0] || strlen(p.terr) + 5 >= sizeof p.tmp) { InterlockedIncrement64(&skipped); return; }
    strcpy_s(p.tmp, p.terr); strcat_s(p.tmp, ".tmp");
    p.hadBefore = WriteTime(p.sav, &p.before);
    auto* scratch = new (std::nothrow) BlockCodec::EncodeScratch;
    if (!scratch) { InterlockedIncrement64(&skipped); return; }
    LARGE_INTEGER f{}, t0{}, t1{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    p.tiles = GuardedWrite(terrain, p.tmp, scratch, &p.bytes);
    QueryPerformanceCounter(&t1);
    delete scratch;
    p.encodeMs = f.QuadPart ? (t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart : 0;
    if (p.tiles <= 0) {
        TerrainSidecar::RemoveFile(p.tmp); InterlockedIncrement64(&skipped);
        if (H) H->log("terrain sidecar: capture failed (%ld); no sidecar for this save", p.tiles);
        return;
    }
    p.active = true;
}
// Return of SaveGame: the .sav exists now. Stamp, rename, sweep.
static void AfterSave(PendingWrite& p) {
    if (!p.active) return;
    FILETIME after{};
    const bool have = WriteTime(p.sav, &after);
    bool fresh = have && (!p.hadBefore || CompareFileTime(&after, &p.before) != 0);
    char resolved[1040]; strcpy_s(resolved, p.sav);
    if (!fresh) {
        // not the file the engine wrote (an autosave): the one it did write in that folder
        char written[1040];
        if (WrittenDuringCall(p.sav, p.callStart, written, sizeof written)) {
            strcpy_s(p.sav, written);
            TerrainSidecar::SidecarPath(p.sav, p.terr, sizeof p.terr);
            fresh = p.terr[0] != 0;
            if (fresh && H) H->log("terrain sidecar: the engine wrote %s, not %s; the sidecar goes beside it", p.sav, resolved);
        }
    }
    uint64_t fp = fresh ? TerrainSidecar::HashFile(p.sav) : 0;
    bool ok = fp && TerrainSidecar::Refingerprint(p.tmp, fp);
    if (ok) {
        wchar_t from[1100], to[1100];
        ok = TerrainSidecar::WidePath(p.tmp, from, 1100) && TerrainSidecar::WidePath(p.terr, to, 1100) &&
             MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!ok) {
        TerrainSidecar::RemoveFile(p.tmp); InterlockedIncrement64(&skipped);
        if (H) H->log("terrain sidecar: the save was not written (or could not be hashed); the captured sidecar is discarded "
                      "(expected %s: %s, %s; no .sav in that folder written during the save)", resolved,
                      have ? "exists" : "absent", !have ? "-" : fresh ? "written" : "unchanged");
        return;
    }
    InterlockedIncrement64(&written);
    const int swept = SweepOrphans(p.sav);
    if (H) H->log("terrain sidecar: wrote %ld tiles, %.1f MiB, capture %lld ms, beside %s%s", p.tiles, double(p.bytes) / 1048576.0, p.encodeMs,
                  p.sav, swept ? " (sidecars of deleted saves removed)" : "");
}

static void* __fastcall SaveDetour(void* ret, void* meta, void* shot, void* cfg, void* res, void* state, void* gui, const SaveGameId* id, bool flag, void* monitor) {
    PendingWrite p; BeforeSave(id, p);
    void* r = originalSave(ret, meta, shot, cfg, res, state, gui, id, flag, monitor);
    AfterSave(p);
    return r;
}
static void* __fastcall LoadDetour(void* ret, void* ctx, void* modRep, const SaveGameId* id, void* mods, void* settings, void* pairs, bool flag, void* s1, void* s2, void* monitor) {
    // A new world: whatever CTerrain the alignment system last saw is gone.
    g_alignmentTerrain = nullptr;
    TerrainServe::ForgetTerrains();
    if (g_terrainServe) {
        char sav[1040];
        if (ResolveSavePath(id, sav, sizeof sav)) {
            LARGE_INTEGER f{}, t0{}, t1{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            TerrainSidecar::ArmForLoad(sav);
            QueryPerformanceCounter(&t1);
            InterlockedIncrement64(&loadsArmed);
            if (H) H->log("terrain sidecar: loading %s, fingerprint %016llx in %lld ms; candidate %s", sav,
                          (unsigned long long)TerrainSidecar::g_saveFingerprint, f.QuadPart ? (t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart : 0,
                          TerrainSidecar::g_sidecarPath[0] ? TerrainSidecar::g_sidecarPath : "(none)");
        } else if (H) H->log("terrain sidecar: this load's save path could not be resolved; loading stock");
    }
    return originalLoad(ret, ctx, modRep, id, mods, settings, pairs, flag, s1, s2, monitor);
}
}  // namespace SidecarIo

static int SidecarIoStatus(char* out, size_t cap);
static bool InstallSidecarIo() {
    using namespace SidecarIo;
    if (!g_terrainServe || g_gog) return false;
    if (!H->verifyBytes(kSaveGameRva, kSaveGameBytes, sizeof kSaveGameBytes) || !H->verifyBytes(kLoadGameRva, kLoadGameBytes, sizeof kLoadGameBytes)) {
        H->log("terrain sidecar: SaveGame/LoadGame byte mismatch; sidecars are neither written nor read"); return false;
    }
    base = H->moduleBase();
    backendOk = H->verifyBytes(kAppRva, kAppBytes, sizeof kAppBytes) && H->verifyBytes(kBackendRva, kBackendBytes, sizeof kBackendBytes) &&
                H->verifyBytes(kSaveDirRva, kSaveDirBytes, sizeof kSaveDirBytes);
    if (!backendOk) H->log("terrain sidecar: the save backend accessors do not match; only saves that carry their own folder get a sidecar");
    inArena = TerrainPager::Contains;
    liveTiles = []() -> uint64_t { return TerrainPager::Snapshot().live; };
    g_sidecarIoStatus = SidecarIoStatus;
    if (!H->installHook(base + kLoadGameRva, reinterpret_cast<void*>(LoadDetour), sizeof kLoadGameBytes, reinterpret_cast<void**>(&originalLoad))) {
        H->log("terrain sidecar: LoadGame hook failed; sidecars are not read"); return false;
    }
    if (g_sidecarWrite && !H->installHook(base + kSaveGameRva, reinterpret_cast<void*>(SaveDetour), sizeof kSaveGameBytes, reinterpret_cast<void**>(&originalSave))) {
        H->log("terrain sidecar: SaveGame hook failed; sidecars are read but not written");
    }
    H->log("terrain sidecar: %s beside each save (max %d grid records), armed at LoadGame by the save's fingerprint",
           originalSave ? "written" : "NOT written", g_sidecarMaxTiles);
    return true;
}
// For the pager's 30 s line.
static int SidecarIoStatus(char* out, size_t cap) {
    using namespace SidecarIo;
    if (!saves && !loadsArmed) { if (cap) out[0] = 0; return 0; }
    return snprintf(out, cap, " sidecar_io: saves=%lld written=%lld skipped=%lld loads_armed=%lld", saves, written, skipped, loadsArmed);
}
// Offline test entry points (no game state).
extern "C" __declspec(dllexport) int BigmapTestSidecarResolve(const void* id, char* out, int cap) {
    return SidecarIo::ResolveSavePath(static_cast<const SidecarIo::SaveGameId*>(id), out, size_t(cap)) ? 1 : 0;
}
extern "C" __declspec(dllexport) int BigmapTestSidecarRefingerprint(const char* path, uint64_t fp) { return TerrainSidecar::Refingerprint(path, fp) ? 1 : 0; }
extern "C" __declspec(dllexport) int BigmapTestSidecarFind(uint64_t fp, char* path, int cap) { return TerrainSidecar::FindByFingerprint(fp, path, size_t(cap)) ? 1 : 0; }
extern "C" __declspec(dllexport) int BigmapTestSidecarSweep(const char* savPath) { return SidecarIo::SweepOrphans(savPath); }
// secondsAgo: the save call started that many seconds before now
extern "C" __declspec(dllexport) int BigmapTestSidecarWrittenDuring(const char* resolved, int secondsAgo, char* out, int cap) {
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t{}; t.LowPart = now.dwLowDateTime; t.HighPart = now.dwHighDateTime;
    t.QuadPart -= uint64_t(secondsAgo) * 10000000ull;
    FILETIME start{}; start.dwLowDateTime = t.LowPart; start.dwHighDateTime = t.HighPart;
    return SidecarIo::WrittenDuringCall(resolved, start, out, size_t(cap)) ? 1 : 0;
}
