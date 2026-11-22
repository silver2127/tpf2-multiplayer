// Steam 35924: a terrain sidecar file, so a save's aligned 1 m height cache is
// restored on load instead of rebuilt by the refine + alignment pass.
//
// The .sav stores the 4 m base heightmap and every road/track/construction
// alignment, NOT the finished 1 m cache. On load the engine rebuilds the cache
// (bicubic refine, terrain_util 0x3c4620) and then re-cuts it with every
// alignment (ecs::TerrainAlignmentSystem, 0xaad6c0). On a 207,360-tile save
// that pass is the load's memory peak and a large share of its time. The
// finished cache is a pure function of the save, so writing it beside the save
// and splatting it back on load of the SAME save is exact.
//
// This header owns the FILE FORMAT and the GRID WALK only. It reads and writes
// the live terrain grid through the layout confirmed by RE (CTerrain::GetTile
// 0x33d580, AddTile 0x33cb60):
//
//   CTerrain* + 0x18            -> grid*
//   grid + 0x00 int32 x0        (window origin, unused here)
//   grid + 0x04 int32 y0
//   grid + 0x08 int32 nx        (record columns)
//   grid + 0x0c int32 ny        (record rows)
//   grid + 0x10 void* records   (nx*ny records, 40 bytes each)
//   record + 0x00 int32 entity  (the tile entity id AddTile stored; not a liveness flag)
//   record + 0x08 std::shared_ptr<std::vector<uint16_t>>: +0x08 the vector object
//                 itself {uint16* first, * last, * end} (nullptr == no tile), +0x10 its
//                 control block (the object - 0x10: make_shared)
//   record + 0x20 int32 version
//
// MEASURED 2026-09-21 in the running game (the dedicated server, /proc/<pid>/mem):
// all 8,712 records hold a 66,049-sample vector at *(record+8)+0, and
// *(record+8) - *(record+0x10) == 0x10 on every one. The two CTerrain versions
// point at the SAME vector objects (copy-on-write). An earlier reading put the
// vector at *(record+8)+0x10: that is the vector's `end` and the next object's
// bytes, so no tile was ever eligible and no sidecar could be written.
// AddTile (0x33cb60) at 0x33cc90 does `lea rcx,[record+8]; call 0x33dd20`
// (detach/make-writable on the shared_ptr, returns the vector) then resizes it to
// Side*Side. A tile is eligible when the pointer is set and that vector holds
// exactly Side*Side samples (66,049 for the 1 m cache).
//
// ApplyTile WRITES the vector. It is called only from the pass owner's AddTile
// post-hook, i.e. straight after AddTile has already detached (0x33dd20) and
// resized the block, so the vector is private and writable and ApplyTile needs
// no engine call. Its `first` may point into the terrain pager's arena; the
// Decode write faults it in as usual. Each is compressed with the block codec.
// The file is
// keyed to the save by a caller-supplied 64-bit fingerprint (the .sav hash);
// Apply refuses a file whose fingerprint or grid dimensions do not match, so a
// foreign, stale or plugin-less save is never touched.
//
// This header does not hook the game. The SaveGame/LoadGame hooks and the
// short-circuit of the alignment pass are integrated separately; see the
// exported test entry points and docs/terrain-sidecar.md.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN   // keeps rpcndr.h's `#define small char` out of whoever includes this
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// Linux portability for the shared codec and experimental native sidecars.
// The file format is unchanged; only file, directory and lock primitives differ.
#include <pthread.h>
#include <dirent.h>
#include <strings.h>
typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
inline void AcquireSRWLockExclusive(SRWLOCK* l) { pthread_rwlock_wrlock(l); }
inline void ReleaseSRWLockExclusive(SRWLOCK* l) { pthread_rwlock_unlock(l); }
inline void AcquireSRWLockShared(SRWLOCK* l) { pthread_rwlock_rdlock(l); }
inline void ReleaseSRWLockShared(SRWLOCK* l) { pthread_rwlock_unlock(l); }
#endif
#include "small_codec.h"
#include "terrain_codec.h"   // Side, Samples

namespace TerrainSidecar {
constexpr uint32_t Magic = 0x52524554;      // 'TERR'
constexpr uint32_t Version = 1;
constexpr size_t Side = TerrainCodec::Side;         // 257
constexpr size_t Samples = TerrainCodec::Samples;   // 66,049

// Paths are UTF-8, as the engine hands them over (its save directory is a
// UTF-8 std::string); the CRT's narrow fopen would read them in the ANSI code
// page and miss a profile folder with a non-ASCII name. Windows only: this
// header is part of the plugin DLL and of its offline test.
#ifdef _WIN32
inline bool WidePath(const char* utf8, wchar_t* out, int cap) {
    if (!utf8 || !utf8[0]) return false;
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap) > 0;
}
inline FILE* OpenFile(const char* utf8, const wchar_t* mode) {
    wchar_t w[1040]; FILE* f = nullptr;
    if (!WidePath(utf8, w, 1040) || _wfopen_s(&f, w, mode)) return nullptr;
    return f;
}
inline void RemoveFile(const char* utf8) { wchar_t w[1040]; if (WidePath(utf8, w, 1040)) _wremove(w); }
#else
// Linux paths are bytes: UTF-8 passes through unchanged.
inline FILE* OpenFile(const char* path, const wchar_t* mode) {
    char m[8]; size_t i = 0;
    for (; mode[i] && i + 1 < sizeof m; ++i) m[i] = char(mode[i]);
    m[i] = 0;
    return path && path[0] ? fopen(path, m) : nullptr;
}
inline void RemoveFile(const char* path) { if (path && path[0]) remove(path); }
#endif

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic, version;
    uint64_t fingerprint;       // the save's content hash (caller-supplied)
    int32_t nx, ny;             // grid dimensions, so a resized world is rejected
    uint32_t tiles;             // eligible tiles stored
    uint64_t headerHash;        // hash of the fields above, for a torn/truncated file
};
struct TileHeader { uint32_t index; uint32_t bytes; };   // record index in the grid, compressed length
#pragma pack(pop)

// The engine's terrain grid, as a raw view (never constructed by us).
struct Grid {
    uint8_t* base;
    int32_t x0() const { return *reinterpret_cast<const int32_t*>(base + 0); }
    int32_t y0() const { return *reinterpret_cast<const int32_t*>(base + 4); }
    int32_t nx() const { return *reinterpret_cast<const int32_t*>(base + 8); }
    int32_t ny() const { return *reinterpret_cast<const int32_t*>(base + 0xc); }
    uint8_t* records() const { return *reinterpret_cast<uint8_t* const*>(base + 0x10); }
    uint8_t* record(uint32_t i) const { return records() + size_t(i) * 40; }
};
// The record index for tile coordinate (x, y), the same value CTerrain::GetTile
// (0x33d580) and AddTile (0x33cb60) compute: (x - x0) + (y - y0) * nx. -1 when
// the coordinate is outside the grid window. This is the index the file stores
// and the argument Has()/ApplyTile() expect. A caller that already holds the
// engine's record pointer can use IndexOfRecord instead.
inline long RecordIndex(const Grid& g, int32_t x, int32_t y) {
    const int32_t x0 = g.x0(), y0 = g.y0(), nx = g.nx(), ny = g.ny();
    if (x < x0 || y < y0 || x >= x0 + nx || y >= y0 + ny) return -1;
    return long(x - x0) + long(y - y0) * nx;
}
inline long IndexOfRecord(const Grid& g, const uint8_t* record) {
    if (!g.records() || record < g.records()) return -1;
    size_t off = size_t(record - g.records());
    if (off % 40) return -1;
    long i = long(off / 40);
    return i < long(g.nx()) * g.ny() ? i : -1;
}
inline Grid GridOf(void* cterrain) { return Grid{*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(cterrain) + 0x18)}; }
struct TileVector { uint16_t* first; uint16_t* last; uint16_t* end; };
// record+8 -> the vector object itself (the shared_ptr's pointer). A nullptr
// (no tile) yields a null vector, which Eligible rejects.
inline TileVector* VectorOf(uint8_t* record) {
    return *reinterpret_cast<TileVector**>(record + 8);
}
inline bool Eligible(const TileVector* v) { return v && v->first && size_t(v->last - v->first) == Samples; }

inline uint64_t MixHash(uint64_t h, uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
inline uint64_t HashHeader(const FileHeader& h) {
    uint64_t v = 0;
    v = MixHash(v, h.magic); v = MixHash(v, h.version); v = MixHash(v, h.fingerprint);
    v = MixHash(v, uint64_t(uint32_t(h.nx))); v = MixHash(v, uint64_t(uint32_t(h.ny))); v = MixHash(v, h.tiles);
    return v;
}

// One tile's encode. On Windows a fault reading the tile (a freed or foreign
// vector) is caught here, per tile and on whichever thread encodes it, and the
// tile is omitted -- the caller's own __try does not reach worker threads.
#ifdef _WIN32
inline size_t EncodeTileGuarded(const uint16_t* first, uint8_t* out, size_t cap, BlockCodec::EncodeScratch* s, bool* fault) {
    __try { return BlockCodec::Encode(first, Samples, out, cap, *s); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *fault = true; return 0; }
}
#else
inline size_t EncodeTileGuarded(const uint16_t* first, uint8_t* out, size_t cap, BlockCodec::EncodeScratch* s, bool*) {
    return BlockCodec::Encode(first, Samples, out, cap, *s);
}
#endif
// Encoder threads for a sidecar write: `requested` when positive, else the
// CPUs less one, at most 8. MEASURED 2026-09-28: 36,992 tiles took 12,170 ms on
// one thread, inside SaveGame, while every player waited on the save.
inline unsigned WriteThreads(int requested) {
    if (requested > 0) return unsigned(requested > 16 ? 16 : requested);
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned n = hw > 1 ? hw - 1 : 1;
    return n > 8 ? 8 : n;
}
static int g_writeThreads = 0;   // terrain_sidecar_threads; 0 = automatic, 1 = the single-thread path

// ---- Write: compress every eligible tile of `grid` to `path`. ----
// Returns the number of tiles written, or -1 on an I/O or allocation failure.
// A partial file is removed on failure so a later Apply never sees it.
// Tiles are encoded in windows of WriteWindow on WriteThreads() threads and
// written in index order, so the file is byte for byte the single-thread one.
constexpr uint32_t WriteWindow = 2048;
inline long Write(const Grid& grid, uint64_t fingerprint, const char* path,
                  BlockCodec::EncodeScratch* scratch, uint64_t* compressedBytesOut = nullptr) {
    if (!grid.base || !grid.records()) return -1;
    const int32_t nx = grid.nx(), ny = grid.ny();
    if (nx <= 0 || ny <= 0 || int64_t(nx) * ny > (int64_t(1) << 24)) return -1;
    FILE* f = OpenFile(path, L"wb");
    if (!f) return -1;
    FileHeader hdr{Magic, Version, fingerprint, nx, ny, 0, 0};
    // Header rewritten at the end with the true tile count and hash.
    if (fwrite(&hdr, sizeof hdr, 1, f) != 1) { fclose(f); RemoveFile(path); return -1; }
    const size_t cap = Samples * 2 + 128;
    uint32_t tiles = 0; uint64_t compressed = 0; bool ok = true;
    const uint32_t count = uint32_t(nx) * uint32_t(ny);
    const unsigned threads = WriteThreads(g_writeThreads);
    // Per-window output: slot k holds tile (window start + k), its length in lens[k]
    // (0 = omitted: not eligible, incompressible, or unreadable).
    const uint32_t window = count < WriteWindow ? count : WriteWindow;
    std::unique_ptr<uint8_t[]> out(new (std::nothrow) uint8_t[size_t(window) * cap]);
    std::unique_ptr<uint32_t[]> lens(new (std::nothrow) uint32_t[window]);
    std::vector<std::unique_ptr<BlockCodec::EncodeScratch>> scratches;
    for (unsigned t = 1; t < threads; ++t) {
        std::unique_ptr<BlockCodec::EncodeScratch> s(new (std::nothrow) BlockCodec::EncodeScratch);
        if (!s) break;
        scratches.push_back(std::move(s));
    }
    if (!out || !lens) { fclose(f); RemoveFile(path); return -1; }
    for (uint32_t start = 0; start < count && ok; start += window) {
        const uint32_t n = count - start < window ? count - start : window;
        std::atomic<uint32_t> next{0};
        auto work = [&](BlockCodec::EncodeScratch* s) {
            for (uint32_t k; (k = next.fetch_add(1)) < n;) {
                lens[k] = 0;
                TileVector* v = VectorOf(grid.record(start + k));
                if (!Eligible(v)) continue;
                bool fault = false;
                const size_t bytes = EncodeTileGuarded(v->first, out.get() + size_t(k) * cap, cap, s, &fault);
                lens[k] = fault ? 0 : uint32_t(bytes);
            }
        };
        std::vector<std::thread> pool;
        for (auto& s : scratches) {
            try { pool.emplace_back(work, s.get()); } catch (...) { break; }   // fewer threads, same result
        }
        work(scratch);
        for (auto& t : pool) t.join();
        for (uint32_t k = 0; k < n && ok; ++k) {
            if (!lens[k]) continue;   // omitted; Apply leaves it to the pass
            TileHeader th{start + k, lens[k]};
            if (fwrite(&th, sizeof th, 1, f) != 1 || fwrite(out.get() + size_t(k) * cap, 1, lens[k], f) != lens[k]) { ok = false; break; }
            ++tiles; compressed += lens[k];
        }
    }
    if (ok) {
        hdr.tiles = tiles; hdr.headerHash = HashHeader(hdr);
        if (fseek(f, 0, SEEK_SET) || fwrite(&hdr, sizeof hdr, 1, f) != 1) ok = false;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { RemoveFile(path); return -1; }
    if (compressedBytesOut) *compressedBytesOut = compressed;
    return long(tiles);
}

// ---- Apply: fill every stored tile of `path` into `grid`. ----
// Returns the number of tiles applied, 0 if the file is absent/foreign/stale
// (the caller then lets the pass run), or -1 on a corrupt-but-matching file
// (the caller must let the pass run and should discard the file). Only writes
// into a record whose vector is already the right size (the tile exists); it
// never allocates a tile, so it runs after the pass has created them.
inline long Apply(const Grid& grid, uint64_t fingerprint, const char* path,
                  BlockCodec::DecodeScratch* scratch) {
    if (!grid.base || !grid.records()) return 0;
    FILE* f = OpenFile(path, L"rb");
    if (!f) return 0;
    FileHeader hdr{};
    if (fread(&hdr, sizeof hdr, 1, f) != 1) { fclose(f); return 0; }
    if (hdr.magic != Magic || hdr.version != Version || hdr.headerHash != HashHeader(hdr)) { fclose(f); return 0; }
    if (hdr.fingerprint != fingerprint || hdr.nx != grid.nx() || hdr.ny != grid.ny()) { fclose(f); return 0; }
    const uint32_t count = uint32_t(grid.nx()) * uint32_t(grid.ny());
    std::vector<uint8_t> blob(Samples * 2 + 128);
    long applied = 0; bool corrupt = false;
    for (uint32_t k = 0; k < hdr.tiles; ++k) {
        TileHeader th{};
        if (fread(&th, sizeof th, 1, f) != 1) { corrupt = true; break; }
        if (th.bytes == 0 || th.bytes > blob.size() || th.index >= count) { corrupt = true; break; }
        if (fread(blob.data(), 1, th.bytes, f) != th.bytes) { corrupt = true; break; }
        TileVector* v = VectorOf(grid.record(th.index));
        if (!Eligible(v)) continue;   // tile not present this load: skip, leave it to the pass
        if (!BlockCodec::Decode(blob.data(), th.bytes, v->first, Samples, *scratch)) { corrupt = true; break; }
        ++applied;
    }
    fclose(f);
    return corrupt ? -1 : applied;
}
}  // namespace TerrainSidecar

// ---- Streaming load-side API, for the alignment pass to consume per tile. ----
// The pass creates each tile (AddTile) and would then compute its cache. When
// a sidecar for this exact save is loaded, the pass owner skips that compute
// for a tile Has(index) reports, and calls ApplyTile to splat the saved cache
// instead. BeginApply verifies EVERY stored blob decodes before reporting any
// tile, so Has(index)==true guarantees ApplyTile(index) succeeds; the pass can
// therefore skip the compute without a fallback. The compressed file is held
// in memory (~1.2 GiB for a full map, far under the pass's own former peak)
// until EndApply. All of BeginApply/EndApply run once; Has/ApplyTile are
// lock-free reads of the immutable index and may run on any pass thread
// (ApplyTile takes the caller's own DecodeScratch).
namespace TerrainSidecar {
// A tile's blob: which piece of the file holds it, where, and how long (0 = absent).
struct Loc { uint32_t chunk = 0, off = 0, bytes = 0; };
struct LoadState {
    // The file as read so far, in pieces: the whole file at once for a sidecar
    // beside the save, one piece per Refresh for a stream still arriving (a
    // piece never moves once read, so a decode needs only the shared lock).
    std::vector<std::vector<uint8_t>> chunks;
    std::vector<Loc> byIndex;                        // record index -> blob
    bool loaded = false;
    uint32_t tiles = 0;                              // the header's tile count
    uint32_t indexed = 0;                            // tiles indexed so far (== tiles once complete)
    uint32_t count = 0;                              // grid records
    // A STREAM (the host's sidecar still downloading, terrain-stream.md): the
    // open file, the bytes read from it, and the partial record at its end.
    FILE* stream = nullptr;
    uint64_t bytes = 0;
    std::vector<uint8_t> tail;
    std::string streamPath;                          // set when this came from a stream (done marker on release)
};
// The load is done with a stream: "<stream>.done" tells the joiner's lobby to
// stop downloading the rest (it would only compete with the game's traffic).
inline void MarkStreamDone(const std::string& path) {
    if (path.empty()) return;
    if (FILE* f = OpenFile((path + ".done").c_str(), L"wb")) fclose(f);
}
static LoadState g_load;
// THREADS (2026-09-26, a player's crash dump: tpf2_bigmap.dll+0x2e74 in
// EndApply's vector free, the block's header page already released). The game
// runs the alignment pass on many threads at once and every batched pass that
// finishes calls EndApply: two of them released the same file. AddTile threads
// may also still be decoding from it. Has/ApplyTile hold this lock shared
// (ApplyTile across its decode); BeginApply, Refresh and EndApply take it
// exclusive, and the file is freed once, outside the lock.
static SRWLOCK g_loadLock = SRWLOCK_INIT;

constexpr uint32_t MaxBlob = 64u << 20;              // a tile's blob is ~16 KB; anything near this is a broken file
// Index every complete record in `piece` (after `st.tail`, the partial record a
// previous read ended in), keeping the piece. False if a record is malformed.
inline bool IndexPiece(LoadState& st, std::vector<uint8_t>&& piece) {
    std::vector<uint8_t> data;
    if (!st.tail.empty()) { data = std::move(st.tail); st.tail.clear(); data.insert(data.end(), piece.begin(), piece.end()); }
    else data = std::move(piece);
    size_t p = 0, end = data.size();
    const uint32_t chunk = uint32_t(st.chunks.size());
    bool valid = true;
    while (st.indexed < st.tiles && p + sizeof(TileHeader) <= end) {
        TileHeader th{}; memcpy(&th, data.data() + p, sizeof th);
        if (th.bytes == 0 || th.bytes > MaxBlob || th.index >= st.count) { valid = false; break; }
        if (p + sizeof(TileHeader) + th.bytes > end) break;          // this record is still arriving
        st.byIndex[th.index] = Loc{chunk, uint32_t(p + sizeof(TileHeader)), th.bytes};
        p += sizeof(TileHeader) + th.bytes;
        ++st.indexed;
    }
    if (valid && st.indexed < st.tiles && p < end) st.tail.assign(data.begin() + p, data.end());
    data.resize(p);
    // Even on a malformed suffix, retain storage for every index published above.
    st.chunks.push_back(std::move(data));
    return valid;
}
// Everything `f` holds from its current position on.
inline int64_t Tell(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return int64_t(ftello(f));
#endif
}
inline bool Seek(FILE* f, int64_t off, int whence) {
#ifdef _WIN32
    return _fseeki64(f, off, whence) == 0;
#else
    return fseeko(f, off_t(off), whence) == 0;
#endif
}
inline std::vector<uint8_t> ReadAvailable(FILE* f) {
    std::vector<uint8_t> out;
    clearerr(f);                                     // a stream's earlier read hit its end
    const int64_t at = Tell(f);
    int64_t end = -1;
    if (at >= 0 && Seek(f, 0, SEEK_END)) { end = Tell(f); Seek(f, at, SEEK_SET); }
    if (end > at) {                                  // one read of what is there now
        out.resize(size_t(end - at));
        out.resize(fread(out.data(), 1, out.size(), f));
    }
    return out;
}

// Open, validate the header against the live grid's fingerprint and dimensions,
// and index every tile by scanning the tile headers (NO decode). Returns the
// number of tiles indexed, or 0 (nothing held) if the file is absent, foreign,
// stale, or structurally broken (a header whose lengths run past the file).
//
// With `stream`, a file still arriving (the host's sidecar being downloaded
// while this load runs) is taken as far as it goes and kept open: Refresh
// indexes what arrives later, and the return is the tiles indexed so far,
// possibly 0 with the header in hand (then it is loaded, just empty yet).
//
// It deliberately does NOT decode the blobs up front: on a 100k-tile map that
// would stall the first AddTile for the whole verify. Each blob carries its own
// content hash, so a bad blob is caught at ApplyTile, which returns false. The
// contract is therefore: the caller marks a tile served (and skips its
// publication) ONLY when ApplyTile returns true; a false result must fall back
// to the normal compute for that tile. `verify` is unused, kept so the caller's
// BeginIfPending(cterrain, scratch) signature is stable.
inline long BeginApply(const Grid& grid, uint64_t fingerprint, const char* path, BlockCodec::DecodeScratch* /*verify*/ = nullptr, bool stream = false) {
    {
        LoadState old;
        AcquireSRWLockExclusive(&g_loadLock);
        old = std::move(g_load);
        g_load = LoadState{};
        ReleaseSRWLockExclusive(&g_loadLock);
        if (old.stream) fclose(old.stream);
        MarkStreamDone(old.streamPath);
    }
    if (!grid.base || !grid.records()) return 0;
    FILE* f = OpenFile(path, L"rb");
    if (!f) return 0;
    FileHeader hdr{};
    if (fread(&hdr, sizeof hdr, 1, f) != 1 || hdr.magic != Magic || hdr.version != Version || hdr.headerHash != HashHeader(hdr) ||
        hdr.fingerprint != fingerprint || hdr.nx != grid.nx() || hdr.ny != grid.ny()) { fclose(f); return 0; }
    LoadState st;
    st.count = uint32_t(grid.nx()) * uint32_t(grid.ny());
    st.tiles = hdr.tiles;
    st.byIndex.assign(st.count, Loc{});
    std::vector<uint8_t> rest = ReadAvailable(f);
    st.bytes = sizeof hdr + rest.size();
    if (hdr.tiles > st.count || !IndexPiece(st, std::move(rest))) { fclose(f); return 0; }
    if (st.indexed == st.tiles) {
        fclose(f);
    } else if (!stream) {
        fclose(f); return 0;                         // truncated
    } else {
        st.stream = f;
    }
    if (stream) st.streamPath = path;
    st.loaded = true;
    const long indexed = long(st.indexed);
    AcquireSRWLockExclusive(&g_loadLock);
    g_load = std::move(st);
    ReleaseSRWLockExclusive(&g_loadLock);
    return stream ? indexed : long(hdr.tiles);
}
// A stream: index what arrived since the last look. True while it is still
// arriving; false once complete (the file is then closed) or not a stream.
// A malformed stream is closed with what it had.
inline bool Refresh() {
    FILE* done = nullptr; bool more = false;
    AcquireSRWLockExclusive(&g_loadLock);
    if (g_load.loaded && g_load.stream) {
        std::vector<uint8_t> piece = ReadAvailable(g_load.stream);
        g_load.bytes += piece.size();
        const bool ok = piece.empty() || IndexPiece(g_load, std::move(piece));
        if (!ok || g_load.indexed == g_load.tiles) { done = g_load.stream; g_load.stream = nullptr; g_load.tail.clear(); }
        else more = true;
    }
    ReleaseSRWLockExclusive(&g_loadLock);
    if (done) fclose(done);
    return more;
}
struct StreamState { bool loaded, streaming; uint32_t indexed, tiles; uint64_t bytes; };
inline StreamState Progress() {
    AcquireSRWLockShared(&g_loadLock);
    const StreamState s{g_load.loaded, g_load.stream != nullptr, g_load.indexed, g_load.tiles, g_load.bytes};
    ReleaseSRWLockShared(&g_loadLock);
    return s;
}
inline bool Loaded() {
    AcquireSRWLockShared(&g_loadLock);
    const bool loaded = g_load.loaded;
    ReleaseSRWLockShared(&g_loadLock);
    return loaded;
}
// Caller holds g_loadLock (shared or exclusive).
inline bool HasLocked(uint32_t recordIndex) {
    return g_load.loaded && recordIndex < g_load.byIndex.size() && g_load.byIndex[recordIndex].bytes != 0;
}
inline bool Has(uint32_t recordIndex) {
    AcquireSRWLockShared(&g_loadLock);
    const bool has = HasLocked(recordIndex);
    ReleaseSRWLockShared(&g_loadLock);
    return has;
}
// Decode the saved cache for `recordIndex` into that tile's height vector.
// False if not loaded, not stored, the tile is not present/eligible, or the
// blob fails to decode (a corrupt blob: the caller must then leave the tile to
// the normal compute, i.e. not mark it served). The block codec's content hash
// makes a bad decode a reliable false, never a wrong restore.
inline bool ApplyTile(const Grid& grid, uint32_t recordIndex, BlockCodec::DecodeScratch& scratch) {
    AcquireSRWLockShared(&g_loadLock);
    bool ok = false;
    if (HasLocked(recordIndex)) {
        TileVector* v = VectorOf(grid.record(recordIndex));
        if (Eligible(v)) {
            const Loc& e = g_load.byIndex[recordIndex];
            ok = BlockCodec::Decode(g_load.chunks[e.chunk].data() + e.off, e.bytes, v->first, Samples, scratch);
        }
    }
    ReleaseSRWLockShared(&g_loadLock);
    return ok;
}
// Release the loaded file. True for the one caller that released it; false when
// nothing was loaded or another thread got there first.
inline bool EndApply() {
    LoadState old;
    AcquireSRWLockExclusive(&g_loadLock);
    const bool had = g_load.loaded;
    old = std::move(g_load);
    g_load = LoadState{};
    ReleaseSRWLockExclusive(&g_loadLock);
    if (old.stream) fclose(old.stream);
    MarkStreamDone(old.streamPath);
    return had;
}

// ---- Fingerprint and the save/load glue. ----
// The fingerprint ties a sidecar to one save: a 64-bit hash of the .sav bytes.
// The aligned terrain is a pure function of those bytes, so an exact hash match
// means the stored caches are correct, and any other save (even the same map
// with different roads) hashes differently and is rejected.
inline uint64_t HashFile(const char* path) {
    FILE* f = OpenFile(path, L"rb");
    if (!f) return 0;
    uint64_t h = 0xCBF29CE484222325ull; uint64_t total = 0;
    uint8_t buf[1 << 16];
    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, f);
        if (!n) break;
        total += n;
        size_t i = 0;
        for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, buf + i, 8); h = TerrainCodec::Rotl(h ^ (w * 0xC2B2AE3D27D4EB4Full), 31) * 0x9E3779B185EBCA87ull; }
        for (; i < n; ++i) h = TerrainCodec::Rotl(h ^ (uint64_t(buf[i]) * 0xC2B2AE3D27D4EB4Full), 27) * 0x9E3779B185EBCA87ull;
    }
    fclose(f);
    if (!total) return 0;              // empty/unreadable -> 0, which callers treat as "no fingerprint"
    h ^= total; h ^= h >> 33; h *= 0xC2B2AE3D27D4EB4Full; h ^= h >> 29;
    return h ? h : 1;                  // never 0 for a real file
}
// A sidecar is written BEFORE its save exists (inside SaveGame, while the world
// is frozen), so it is first written with fingerprint 0 and stamped afterwards
// with the hash of the .sav the engine produced. False if the file is not a
// sidecar or cannot be rewritten.
inline bool Refingerprint(const char* path, uint64_t fingerprint) {
    FILE* f = OpenFile(path, L"r+b");
    if (!f) return false;
    FileHeader hdr{};
    bool ok = fread(&hdr, sizeof hdr, 1, f) == 1 && hdr.magic == Magic && hdr.version == Version && hdr.headerHash == HashHeader(hdr);
    if (ok) {
        hdr.fingerprint = fingerprint; hdr.headerHash = HashHeader(hdr);
        ok = !fseek(f, 0, SEEK_SET) && fwrite(&hdr, sizeof hdr, 1, f) == 1;
    }
    if (fclose(f) != 0) ok = false;
    return ok;
}
// The fingerprint a sidecar file carries, 0 if it is not a valid sidecar.
inline uint64_t FingerprintOfFile(FILE* f) {
    if (!f) return 0;
    FileHeader hdr{};
    bool ok = fread(&hdr, sizeof hdr, 1, f) == 1 && hdr.magic == Magic && hdr.version == Version && hdr.headerHash == HashHeader(hdr);
    fclose(f);
    return ok ? hdr.fingerprint : 0;
}
#ifdef _WIN32
inline uint64_t FingerprintOf(const wchar_t* widePath) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, widePath, L"rb") || !f) return 0;
    return FingerprintOfFile(f);
}
#endif
inline uint64_t FingerprintOfPath(const char* path) { return FingerprintOfFile(OpenFile(path, L"rb")); }
// The sidecar for a save is normally "<save>.terr". Multiplayer loads a COPY of
// the host's save under another name (mp_shared.sav), and the copy has the same
// bytes and so the same fingerprint: when the named file is absent or carries
// another fingerprint, look through the save's folder for one that matches.
// Reads 32 bytes per candidate. Leaves `path` alone when nothing matches.
#ifdef _WIN32
inline bool FindByFingerprint(uint64_t fingerprint, char* path, size_t cap) {
    wchar_t w[1040];
    if (!fingerprint || !WidePath(path, w, 1040)) return false;
    if (FingerprintOf(w) == fingerprint) return true;
    wchar_t* slash = wcsrchr(w, L'\\'); wchar_t* fwd = wcsrchr(w, L'/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    if (!slash) return false;
    slash[1] = 0;
    wchar_t pattern[1040]; wcscpy_s(pattern, w); wcscat_s(pattern, L"*.terr");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        wchar_t cand[1040]; wcscpy_s(cand, w); wcscat_s(cand, fd.cFileName);
        if (FingerprintOf(cand) == fingerprint) {
            found = WideCharToMultiByte(CP_UTF8, 0, cand, -1, path, int(cap), nullptr, nullptr) > 0;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}
#else
inline bool FindByFingerprint(uint64_t fingerprint, char* path, size_t cap) {
    if (!fingerprint || !path || !path[0]) return false;
    if (FingerprintOfPath(path) == fingerprint) return true;
    const char* slash = strrchr(path, '/');
    if (!slash) return false;
    std::string dir(path, size_t(slash - path + 1));
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool found = false;
    while (struct dirent* e = readdir(d)) {
        const size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 5, ".terr")) continue;
        const std::string cand = dir + e->d_name;
        if (FingerprintOfPath(cand.c_str()) == fingerprint && cand.size() < cap) {
            memcpy(path, cand.c_str(), cand.size() + 1);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}
#endif
inline void SidecarPath(const char* savPath, char* out, size_t cap) {
    // "<save>.sav" -> "<save>.terr"; otherwise "<path>.terr".
    size_t n = strlen(savPath);
#ifdef _WIN32
    const char* ext = (n >= 4 && !_stricmp(savPath + n - 4, ".sav")) ? savPath + n - 4 : savPath + n;
#else
    const char* ext = (n >= 4 && !strcasecmp(savPath + n - 4, ".sav")) ? savPath + n - 4 : savPath + n;
#endif
    size_t base = size_t(ext - savPath);
    if (base + 6 >= cap) { if (cap) out[0] = 0; return; }
    memcpy(out, savPath, base); memcpy(out + base, ".terr", 6);
}

static uint64_t g_saveFingerprint = 0;
static char g_sidecarPath[520] = {0};
static bool g_pending = false;         // a fingerprint is armed; BeginApply not yet run this load (set on
                                       // the load thread before the pass, read once at its entry)

// Called from the LoadGame hook once the .sav path is known, before the world
// builds. Hashes the save and arms the sidecar; the actual BeginApply waits for
// the CTerrain, which BeginIfPending supplies.
static char g_streamDir[520] = {0};    // with a trailing separator; empty = no streams
static bool g_streamWanted = false;    // this load has no sidecar of its own: look for a stream
static bool g_readLocal = true;        // terrain_sidecar_read_local=0: streams only (a test of the stream on one PC,
                                       // where a second instance sees the first one's save folder)
inline void ArmForLoad(const char* savPath) {
    g_saveFingerprint = HashFile(savPath);
    SidecarPath(savPath, g_sidecarPath, sizeof g_sidecarPath);
    if (!g_readLocal) g_sidecarPath[0] = 0;
    else if (g_saveFingerprint && g_sidecarPath[0]) FindByFingerprint(g_saveFingerprint, g_sidecarPath, sizeof g_sidecarPath);
    g_pending = g_saveFingerprint && (g_sidecarPath[0] || g_streamDir[0]);
    g_streamWanted = false;
}

// ---- The host's sidecar, streamed while this load runs (docs/terrain-stream.md) ----
// A joiner's lobby writes the host's "<save>.terr" into <data>/terrain_stream/
// as it downloads, in order, from the moment the load starts. No sidecar beside
// the save: the stream with this save's fingerprint is used instead, as far as
// it has arrived, and the alignment pass waits for the rest when that is faster
// than computing the tiles it lacks.
#ifdef _WIN32
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif
inline void SetStreamDir(const char* dataDir) {
    g_streamDir[0] = 0;
    if (!dataDir || !dataDir[0]) return;
    const size_t n = strlen(dataDir);
    const bool sep = dataDir[n - 1] == '/' || dataDir[n - 1] == '\\';
    snprintf(g_streamDir, sizeof g_streamDir, "%s%sterrain_stream%c", dataDir, sep ? "" : (kSep == '/' ? "/" : "\\"), kSep);
}
inline int64_t NowMs() {
    return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// The stream for `fingerprint` in the stream folder, if its header has arrived.
inline bool FindStream(uint64_t fingerprint, char* path, size_t cap) {
    if (!g_streamDir[0] || !fingerprint || cap < 8) return false;
    const int n = snprintf(path, cap, "%s-.terr", g_streamDir);   // no such file: FindByFingerprint scans its folder
    if (n <= 0 || size_t(n) >= cap) return false;
    return FindByFingerprint(fingerprint, path, cap) && strcmp(path + strlen(path) - 6, "-.terr") != 0;
}
// Load the stream for this save, if there is one yet. Throttled to every 250 ms:
// AddTile and the pass call it freely. True once anything is loaded.
inline bool TryStream(void* cterrain, bool now = false) {
    if (Loaded()) return true;
    if (!g_streamWanted || !cterrain) return false;
    static std::atomic<int64_t> lastMs{0};
    const int64_t t = NowMs();
    int64_t last = lastMs.load();
    if (!now && (t - last < 250 || !lastMs.compare_exchange_strong(last, t))) return false;
    char path[520];
    if (!FindStream(g_saveFingerprint, path, sizeof path)) return false;
    g_streamWanted = false;
    BeginApply(GridOf(cterrain), g_saveFingerprint, path, nullptr, true);
    if (!Loaded()) return false;
    snprintf(g_sidecarPath, sizeof g_sidecarPath, "%s", path);
    return true;
}
// A stream still arriving: index what came since, at most every 100 ms.
inline void RefreshIfDue() {
    static std::atomic<int64_t> lastMs{0};
    const int64_t t = NowMs();
    int64_t last = lastMs.load();
    if (t - last < 100 || !lastMs.compare_exchange_strong(last, t)) return;
    Refresh();
}
// At the alignment pass, with `missing` of this terrain's tiles not yet served:
// wait for the rest of the stream while its arrival (at the rate seen during
// the wait) beats the pass computing them (`msPerTile` each), giving up after
// `stallMs` without a byte or `capMs` in all. True once the stream is complete.
// `why` gets a short account for the log.
inline bool WaitForStream(uint32_t missing, double msPerTile, int64_t stallMs, int64_t capMs, char* why, size_t whyCap) {
    const int64_t t0 = NowMs();
    int64_t lastGrowth = t0;
    StreamState s = Progress();
    const uint64_t bytes0 = s.bytes;
    uint64_t lastBytes = s.bytes;
    for (;;) {
        Refresh();
        s = Progress();
        const int64_t t = NowMs(), waited = t - t0;
        if (!s.streaming) {
            snprintf(why, whyCap, "%s after %lld ms; %u of %u tiles", s.indexed == s.tiles ? "complete" : "ended", (long long)waited, s.indexed, s.tiles);
            return s.loaded && s.indexed == s.tiles;
        }
        if (s.bytes != lastBytes) { lastBytes = s.bytes; lastGrowth = t; }
        if (t - lastGrowth > stallMs) { snprintf(why, whyCap, "no data for %lld ms; %u of %u tiles", (long long)stallMs, s.indexed, s.tiles); return false; }
        if (waited > capMs) { snprintf(why, whyCap, "gave up after %lld ms; %u of %u tiles", (long long)waited, s.indexed, s.tiles); return false; }
        if (waited >= 1000 && s.indexed) {
            // After a second of arrival: the rest at this rate against the pass.
            const double rate = double(s.bytes - bytes0) / double(waited);          // bytes per ms
            const double perTile = double(s.bytes) / double(s.indexed);
            const double eta = rate > 0 ? double(s.tiles - s.indexed) * perTile / rate : 1e18;
            const double pass = double(missing) * msPerTile;
            if (eta > pass) {
                snprintf(why, whyCap, "the rest (%u tiles) would take ~%.0f s at %.1f MB/s, the pass ~%.0f s; %u of %u tiles",
                         s.tiles - s.indexed, eta / 1000.0, rate / 1000.0, pass / 1000.0, s.indexed, s.tiles);
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
// Called with the CTerrain the pass will populate, before its first AddTile
// (the pass owner's Detour entry is the natural spot). Runs BeginApply once.
// Returns true if a valid sidecar is now loaded.
inline bool BeginIfPending(void* cterrain, BlockCodec::DecodeScratch* verify) {
    if (!g_pending || !cterrain) return Loaded();
    g_pending = false;
    if (BeginApply(GridOf(cterrain), g_saveFingerprint, g_sidecarPath, verify) > 0) return true;
    g_streamWanted = g_streamDir[0] != 0;            // none beside the save: the host's may be arriving
    return TryStream(cterrain, true);
}
// Called from the SaveGame hook after the save is written, with the CTerrain and
// the .sav just written. Hashes the save and writes the sidecar beside it.
inline long WriteForSave(void* cterrain, const char* savPath, BlockCodec::EncodeScratch* scratch, uint64_t* bytesOut = nullptr) {
    uint64_t fp = HashFile(savPath);
    if (!fp || !cterrain) return -1;
    char path[520]; SidecarPath(savPath, path, sizeof path);
    if (!path[0]) return -1;
    return Write(GridOf(cterrain), fp, path, scratch, bytesOut);
}
}  // namespace TerrainSidecar

// ---- Offline test entry points (no game state). ----
#ifdef _WIN32
extern "C" __declspec(dllexport) long BigmapTestSidecarWrite(void* cterrain, uint64_t fp, const char* path, uint64_t* bytesOut) {
    auto* s = new BlockCodec::EncodeScratch;
    long r = TerrainSidecar::Write(TerrainSidecar::GridOf(cterrain), fp, path, s, bytesOut);
    delete s; return r;
}
extern "C" __declspec(dllexport) long BigmapTestSidecarApply(void* cterrain, uint64_t fp, const char* path) {
    auto* s = new BlockCodec::DecodeScratch;
    long r = TerrainSidecar::Apply(TerrainSidecar::GridOf(cterrain), fp, path, s);
    delete s; return r;
}
#endif
