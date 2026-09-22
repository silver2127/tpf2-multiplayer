// tpf2_bigmap -- maps larger than the game will build on its own.
//
// WHAT THE GAME DOES
// The New Game menu turns (size dropdown, ratio dropdown) into a tile count via
//
//     CVec2i UI::`anonymous-namespace'::GetNumTilesNew(int sizeIndex,
//                                                      int formatIndex,
//                                                      const AppConfig&)
//     RVA 0x674aa0, MenuUI.cpp:264-273  (build 35924)
//
// and the caller immediately expands that to a heightmap:
//
//     dim = 1 << terrainLevels          (= 64)
//     heightmapPx = tiles * dim + 1
//
// so ONE TILE IS 256 m (64 px at exactly 4.0 m/px). Confirmed twice: against real
// saves (the .sav header after zstd carries numTilesX/numTilesY at +0x10/+0x14;
// Megalomaniac 1:4 reads (48, 192) = 12288 x 49152 m), and against the live bbox
// the streets pass hands us: 224 tiles -> 57344 m = 224 * 256 exactly.
// The wiki's "24 km" for Megalomaniac is 24576 m rounded down.
//
// WHY A HOOK AND NOT A BYTE PATCH
// GetNumTilesNew has a hard clamp of 224 tiles per axis:
//
//     0x674afa:  B9 E0 00 00 00     mov ecx, 0xE0        ; 224
//                cmp edx, ecx / cmovle ...               ; both axes
//
// You could raise that immediate, but a detour is strictly better: it replaces
// the clamp AND the size lookup in one place, never executes the clamp at all
// for the sizes we care about, and leaves every stock size untouched because we
// call the original for anything we do not claim.
//
// There is also a settings.lua route -- `newGameMenuState.worldDimensionsOverride`
// is a shipped, undocumented escape hatch read at AppConfig+0x28/+0x2c which
// bypasses the preset table entirely. It is subject to the same 224 clamp, so it
// tops out at 56 x 56 km. Use it to sanity-check the game's own behaviour; use
// this plugin to go past it.
//
// LIMITS, MEASURED AND INFERRED
//   180 tiles = 46.1 km  largest even size the STOCK 1 m street raster survives
//   224 tiles = 57.3 km  the game's own clamp; needs street_raster=1 (MEASURED OK)
// 224 tiles with the raster hook is measured working, streets included.
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include "tpf2mp_plugin.h"

#pragma intrinsic(_ReturnAddress)

static const Tpf2mpHost* H = nullptr;

// ---------------------------------------------------------------------------
// Target builds
// ---------------------------------------------------------------------------
// Every RVA and EXPECTED byte string here is measured on one of TWO binaries
// that share the same code shape (identical prologues at every hook site):
//   * Steam build 35924 (2024-12-11) -- the one host->buildOk() knows
//   * GOG build         (2024-12-12) -- same code, shifted RVAs; the octree
//     site differs only in the RIP displacement to its .rdata 32768.0f
// g_gog is set once in Tpf2mpPluginInit by byte-verifying all three sites at
// their GOG RVAs. A build that is neither fails that check and is refused
// loudly. Do not add a third build without measuring every site on it.
static bool g_gog = false;

// GetNumTilesNew: the (size, ratio) dropdowns -> tile count (detoured).
static const uintptr_t RVA_GETNUMTILES     = 0x674aa0;   // Steam 35924
static const uintptr_t RVA_GETNUMTILES_GOG = 0x674CC0;   // GOG 2024-12-12
static const int       STEAL               = 20;

// The exact prologue, so a shifted RVA is a loud refusal instead of a jmp into
// the middle of some other instruction. 20 bytes lands on an instruction
// boundary (the next is `mov [r11+8], rbx`) and none of it is RIP-relative, so
// it relocates verbatim into the trampoline.
//
//   89 54 24 10              mov  [rsp+0x10], edx
//   4C 8B DC                 mov  r11, rsp
//   57                       push rdi
//   48 83 EC 70              sub  rsp, 0x70
//   49 C7 43 A8 FE FF FF FF  mov  qword [r11-0x58], -2
static const uint8_t EXPECTED[STEAL] = {
    0x89, 0x54, 0x24, 0x10, 0x4C, 0x8B, 0xDC, 0x57,
    0x48, 0x83, 0xEC, 0x70, 0x49, 0xC7, 0x43, 0xA8,
    0xFE, 0xFF, 0xFF, 0xFF,
};

// ---------------------------------------------------------------------------
// The street occupancy raster (RVA 0x90d410)
// ---------------------------------------------------------------------------
// "Creating streets" allocates a std::vector<bool> with ONE BIT PER SQUARE
// METRE across the whole map bounding box, and sizes it with a 32-bit multiply.
// That is the int32 overflow described above: >46.1 km square and it aborts.
//
// The fix is NOT to widen the multiply. nx and ny are stored as int32 at
// +0x40/+0x44 and every access computes an index like y*nx + x, so a correctly
// sized vector would still be addressed with wrapped negative indices past
// 2^31 cells -- silent corruption instead of a clean abort, which is worse.
//
// Instead we change the INPUT. cellSize is an argument (xmm2), so scaling it
// with map size shrinks nx and ny themselves and every downstream int32 index
// stays in range untouched. No audit, no corruption risk.
//
//   ctor(void* this /*rcx*/, const CVec4f* bbox /*rdx*/, float cellSize /*xmm2*/)
//   bbox = { minX, minY, maxX, maxY }        (read at +0x00/+0x04/+0x08/+0x0c)
//   this+0x08 bbox copy, +0x18 cellSize, +0x20 vector<bool>, +0x40 nx, +0x44 ny
//
//   24.6 km stock : 1 m -> 0.60e9 cells (28% of INT_MAX)  -- untouched
//   57.3 km       : 2 m -> 0.82e9 cells                   -- 411 MB -> 103 MB
//   114.7 km      : 3 m -> 1.46e9 cells
//
// Below the threshold this is a no-op: stock maps keep their 1 m raster and
// behave exactly as before.
static const uintptr_t RVA_RASTER     = 0x90d410;   // Steam 35924
static const uintptr_t RVA_RASTER_GOG = 0x90D500;   // GOG 2024-12-12
static const int       STEAL_RASTER   = 19;

//   48 89 4C 24 08           mov   [rsp+8], rcx
//   53                       push  rbx
//   48 83 EC 30              sub   rsp, 0x30
//   48 C7 44 24 20 FE..FF    mov   qword [rsp+0x20], -2
static const uint8_t EXPECTED_RASTER[STEAL_RASTER] = {
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x30,
    0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,
};

// ---------------------------------------------------------------------------
// OCTREE ROOT BOX -- the 32,768 m wall.
//
// ecs::OctreeSystem (every street node, construction and vehicle lives in it)
// gets its root box from a two-tier CONSTANT, never from the map:
//     tiles <= 128  ->  +-16,384 m, depth  9
//     tiles  > 128  ->  +-32,768 m, depth 10      (= exactly 256 tiles)
// Inserts never test containment, so an entity past the box just walks to the
// boundary leaf. Every query prunes on node boxes FIRST, so anything beyond
// ~32,832 m is invisible to lookups -- and the street builder, finding nothing
// there, stacks a new node on top of the old one. MEASURED on a 320-tile map:
// 21 duplicate-node positions, all with max(|x|,|y|) in [34,175 .. 40,082],
// not one inside 32,768. Towns in that band never get streets (zero pop).
//
// Default fix: widen the >128 tier to +-65,536 m, depth 11. Depth 11 caps the
// STOCK ID scheme; octree_depth12.h supplies an experimental replacement for
// level-11/12 IDs, allowing actual depth 12/13 with the same 128 m leaves.
// node indices are int32 linear (child = 8*parent + 1 + octant) and the
// renderer's skip-manager decoder overflows at level 11+. The leaf stays 128 m
// so query granularity is unchanged; the cost is one extra level per occupied
// path. Default ceiling: 512 tiles (131 km). See docs/octree-depth12.md for
// the opt-in 1024/2048-tile modes and their offline-only validation status.
//
// Site: RVA 0x2304f8, the sole caller of Octree::Resize for the >128 tier
// (reached from InitNewGame, CGame::Load, GameState::Load AND
// GameState::Replicate -- so the multiplayer replicate path is covered too):
//     f3 0f 10 15 98 4c d3 02   movss xmm2, [rip+0x2d34c98]   ; 32768.0f
//     ba 0a 00 00 00            mov   edx, 0xa                ; depth 10
// The 32768.0f lives in a {64, 16384, 32768, FLT_MAX, -90} run in .rdata with
// ~100 readers, so it is NOT touched; the value goes inline as an immediate:
//     b8 00 00 80 47            mov   eax, 0x47800000         ; 65536.0f
//     66 0f 6e d0               movd  xmm2, eax
//     31 d2                     xor   edx, edx
//     b2 0b                     mov   dl, 0xb                 ; depth 11
// eax is dead at that point (the earlier result is already in rbx/[rbp+7] and
// the call 0x11 bytes later clobbers it). Same 13 bytes, so nothing shifts.
//
// Only applied when the config asks for a size over 256 tiles: below that the
// stock box already contains the whole map, and the one thing NOT yet verified
// at depth 11 is the renderer's per-level skip vector sizing -- no reason to
// expose stock-size maps to that.
static const uintptr_t RVA_OCTREE     = 0x2304f8;   // Steam 35924
static const uintptr_t RVA_OCTREE_GOG = 0x230718;   // GOG 2024-12-12
static const uint8_t EXPECTED_OCTREE[13] = {
    0xf3,0x0f,0x10,0x15,0x98,0x4c,0xd3,0x02,   // movss xmm2,[rip+0x2d34c98]  Steam
    0xba,0x0a,0x00,0x00,0x00                   // mov edx,0xa
};
static const uint8_t EXPECTED_OCTREE_GOG[13] = {
    0xf3,0x0f,0x10,0x15,0x88,0x0a,0xd2,0x02,   // movss xmm2,[rip+0x2d20a88]  GOG
    0xba,0x0a,0x00,0x00,0x00                   // mov edx,0xa
};
static const uint8_t PATCH_OCTREE[13] = {
    0xb8,0x00,0x00,0x80,0x47,                  // mov eax,0x47800000 (65536.0f)
    0x66,0x0f,0x6e,0xd0,                       // movd xmm2,eax
    0x31,0xd2,                                 // xor edx,edx
    0xb2,0x0b                                  // mov dl,0xb (depth 11)
};
static const int OCTREE_STOCK_TILES  = 256;    // what the shipped box holds
static const int OCTREE_PATCH_TILES  = 512;    // what the patched box holds
static bool g_octreeOn = true;

#include "octree_depth12.h"
#include "placement_distance.h"
#include "world_entry.h"
#include "material_index.h"
#include "terrain_minmax.h"
#include "terrain_refine.h"
#include "terrain_align_fast.h"
#include "terrain_cache.h"
#include "terrain_compression.h"
#include "terrain_blocks.h"
#include "material_compression.h"
#include "save_fast.h"
#include "travel_time.h"
#include "alignment_batch.h"
#include "terrain_sidecar.h"
#include "terrain_serve.h"
#include "terrain_sidecar_io.h"
#include "instance_shrink.h"

typedef void* (__fastcall *RasterCtorFn)(void* self, const float* bbox, float cellSize);
static RasterCtorFn g_origRaster = nullptr;

// Cell budget. The hard wall is INT_MAX (2.147e9) for the resize, but every
// downstream index is int32 too, so leave real headroom rather than sitting
// just under the cliff.
static double g_cellBudget = 1.5e9;
static int    g_rasterOn   = 1;

// CVec2i is 8 bytes and trivially copyable, so it comes back packed in rax:
// x in the low 32 bits, y in the high 32. Verified at the call site
// (0x140654bc6): `mov rbx, rax` then `mov ecx, ebx` / `shr r15, 0x20`.
typedef uint64_t (__fastcall *GetNumTilesFn)(int sizeIndex, int formatIndex, void* cfg);
static GetNumTilesFn g_orig = nullptr;

// ---------------------------------------------------------------------------
// Config  (section [tpf2_bigmap] in tpf2mp.cfg)
// ---------------------------------------------------------------------------
static int g_tilesX      = 0;     // 0 = plugin does nothing
static int g_tilesY      = 0;
static int g_sizeIndex   = 6;     // which dropdown entry we take over
static int g_formatIndex = 0;     // 0 = 1:1

// ---------------------------------------------------------------------------
// A LADDER, not a single size.
//
// GetNumTilesNew is asked for every (sizeIndex 0..6, formatIndex 0..4) pair, so
// the two dropdowns the game already draws are a 7x5 grid we can answer however
// we like. Claiming one cell gives one big map and no choice; claiming a ROW
// turns the ratio dropdown into a size selector, which is a usable UI without
// adding a single widget.
//
// Config form, one key per claimed cell:
//     size<S>_format<F> = <tilesX>x<tilesY>
// e.g.  size6_format0 = 96x96      (the stock Megalomaniac 1:1, 24.6 x 24.6 km)
//       size6_format1 = 160x160    (41.0 x 41.0 km)
//       size6_format2 = 224x224    (57.3 x 57.3 km)
//
// Anything not claimed falls through to the game's own function, so every other
// preset keeps its stock behaviour exactly.
//
// The labels still say "1:1 / 1:2 / 1:3", which will not match what they now
// produce. That is a real wart and it is why the mapping is config-driven and
// documented rather than hardcoded: whether the strings can be changed cheaply
// is still being established, and until then the honest thing is to keep the
// mapping in one visible place.
struct Claim { int size, format, tx, ty; };
static const int MAX_RATIOS = 20;
static int g_maxRatio = MAX_RATIOS;
static const int MAX_CLAIMS = 19 * MAX_RATIOS;
static Claim g_claims[MAX_CLAIMS];

// ---------------------------------------------------------------------------
// NEW GAME MENU (Steam 35924 only; the GOG build skips it)
//
// Two additions to the vanilla New Game page, both inside dropdowns it already
// draws; nothing adds a widget.
//
// SIZE ROWS (cfg add_size_rows=1). The size dropdown is built at exactly one
// call site, 0x14066ce79 -> combo factory 0x142326290, from a vector<string>:
// base_mod.lua's four names (Small..Very Large) with experimental map sizes
// OFF, the C++'s own seven (Tiny..Megalomaniac) with it ON. The factory hook
// acts only for that call's return address, records the stock row count and
// appends our labels. All three GetNumTilesNew callers pass the RAW combo
// index, so the detour reads index >= stock count as added row (index - stock)
// and answers it before the engine's own lookup -- which, with the flag off,
// would add 1 and build a stock preset, or assert past 6.
//
// DENSITY LEVELS (cfg newgame_density=1). Extra levels after "Very high" in the
// stock Towns, Number of industries and Industry density target lists. Those
// lists are base_mod.lua's own param values, so the labels go in there (the
// base_mod.lua patcher further down), and each list's consumer learns the new
// indices:
//   * Number of industries and the target reach runFn as indices into
//     industryFreq { .4, .6, .8, 1.0 }; the patch appends one multiplier per
//     level.
//   * Towns never reaches Lua. The preview/generation params refresh
//     (0x14065b620) maps the Towns index inline,
//         0 -> 0.2, 1 -> 0.3, 2 -> 0.4, 3 -> 0.5, anything else -> 1.0,
//     into the params object Start generates from (Start only logs the index,
//     and no other code in the exe maps it). The "3 / anything else" tail of
//     that switch is redirected to a stub that answers 3 -> 0.5 as before, our
//     levels -> their multipliers, and anything else -> 1.0 as before.
//
// The size rows' vector is rebuilt by the engine's own vector<string> range
// constructor (0x14009d800) and the old one released by its own destructor
// (0x1400bbd90), so each allocation and free pairs inside the game's CRT. The
// strings we feed in are hand-built MSVC std::string layouts; the constructor
// copies from them and never frees them.
static const uintptr_t RVA_COMBO_FAC         = 0x2326290;  // combo from vector<string> items
static const uintptr_t RET_SIZE_COMBO        = 0x66ce7e;   // after the size combo call
static const uintptr_t RVA_STRVEC_CTOR       = 0x9d800;    // vector<string>(first, last)
static const uintptr_t RVA_STRVEC_DTOR       = 0xbbd90;    // ~vector<string>
static const uintptr_t RVA_TOWN_SWITCH_HEAD  = 0x65b6df;   // loads the Towns index into ecx
static const uintptr_t RVA_TOWN_SWITCH_TAIL  = 0x65b711;   // case 3 and the default: redirected
static const uintptr_t RVA_TOWN_SWITCH_STORE = 0x65b728;   // movss [rsp+0x34], xmm0: every case lands here

//   40 53 / 48 83 EC 40 / 48 C7 44 24 38 FE FF FF FF   push rbx; sub rsp,0x40; mov [rsp+0x38],-2
static const uint8_t EXPECT_COMBO_FAC[15] = {
    0x40,0x53,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x38,0xFE,0xFF,0xFF,0xFF };
//   48 89 4C 24 08 / 41 56 / 48 83 EC 40 / 48 C7 44 24 20 FE FF FF FF
static const uint8_t EXPECT_STRVEC_CTOR[20] = {
    0x48,0x89,0x4C,0x24,0x08,0x41,0x56,0x48,0x83,0xEC,0x40,
    0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF };
//   push rbx; sub rsp,0x20; mov rbx,rcx; mov rcx,[rcx]; test rcx,rcx  (called, never hooked)
static const uint8_t EXPECT_STRVEC_DTOR[15] = {
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0x09,0x48,0x85,0xC9 };
// The E8 call in front of the gated return address, so the gate can only ever
// match the one call it was measured on.
static const uint8_t CALL_SIZE_COMBO[5] = { 0xE8,0x12,0x94,0xCB,0x01 };
// The Towns switch in the params refresh:
//   8B 88 60 04 00 00        mov   ecx, [rax+0x460]     ; the Towns combo's selected index
//   85 C9 / 75 0A            test  ecx, ecx / jne case 1
static const uint8_t EXPECT_TOWN_SWITCH_HEAD[10] = {
    0x8B,0x88,0x60,0x04,0x00,0x00,0x85,0xC9,0x75,0x0A };
//   83 F9 03                 cmp   ecx, 3
//   75 0A                    jne   default
//   C7 44 24 34 00 00 00 3F  mov   dword [rsp+0x34], 0.5f
//   EB 0E                    jmp   past the store
//   F3 0F 10 05 64 32 8C 02  movss xmm0, [rip -> 1.0f]   ; default
//   F3 0F 11 44 24 34        movss [rsp+0x34], xmm0       ; the store, KEPT
// The first 23 bytes become a 14-byte absolute jmp and 9 NOPs; the store stays,
// because the 0.2/0.3/0.4 cases jump straight to it.
static const uint8_t EXPECT_TOWN_SWITCH_TAIL[29] = {
    0x83,0xF9,0x03,0x75,0x0A,0xC7,0x44,0x24,0x34,0x00,0x00,0x00,0x3F,0xEB,0x0E,
    0xF3,0x0F,0x10,0x05,0x64,0x32,0x8C,0x02,0xF3,0x0F,0x11,0x44,0x24,0x34 };

struct MsvcString { char buf[16]; uint64_t size; uint64_t cap; };            // std::string
struct StrVec     { MsvcString* first; MsvcString* last; MsvcString* end; };  // vector<string>
static_assert(sizeof(MsvcString) == 0x20 && sizeof(StrVec) == 0x18, "MSVC x64 std layouts");

typedef void* (__fastcall *ComboFacFn)(StrVec* items, void* onChange, int initialSel, void* aux, int p5);
typedef void* (__fastcall *StrVecCtorFn)(StrVec* out, const MsvcString* first, const MsvcString* last, void* a4);
typedef void  (__fastcall *StrVecDtorFn)(StrVec* vec);

static uintptr_t    g_base       = 0;
static ComboFacFn   g_origCombo  = nullptr;
static StrVecCtorFn g_strVecCtor = nullptr;
static StrVecDtorFn g_strVecDtor = nullptr;

static int  g_addSizeRows  = 1;
static bool g_sizeRowsLive = false;
static volatile long g_stockSizeRows = 7;   // rows the last size combo was built with
struct ExtraRow { int size; char label[16]; };
static ExtraRow   g_extraSizeRows[12];
static MsvcString g_extraRowStrings[12];
static int        g_numExtraSizeRows = 0;

// The extra density levels, in list order after "Very high". Each scale is
// relative to the stock Medium level (towns x0.3, industries x0.6), which is
// what the README's counts assume. This one list feeds all three places: the
// labels added to base_mod.lua, the multipliers appended to industryFreq, and
// the town multipliers the switch stub answers.
struct DensityLevel { const char* label; double scale; };
static const DensityLevel DENSITY_LEVELS[] = {
    { "Reduced (x0.50)",                       0.50  },
    { "Sparse (x0.30)",                        0.30  },
    { "Megalomaniac count at 56 km (x0.18)",   0.18  },
    { "Minimal (x0.10)",                       0.10  },
    { "Megalomaniac count at 112 km (x0.046)", 0.046 },
    { "Megalomaniac count at 160 km (x0.022)", 0.022 },
};
static const int    NUM_DENSITY_LEVELS = (int)(sizeof DENSITY_LEVELS / sizeof DENSITY_LEVELS[0]);
static const double TOWN_MEDIUM        = 0.3;   // the Towns switch's case 1
static const double INDUSTRY_MEDIUM    = 0.6;   // industryFreq[2]
static int   g_newgameDensity = 1;
static float g_townMult[8];                      // read by the stub, so it lives as long as the process
static int   g_numClaims = 0;

// Parse "<w>x<h>", tolerating "<w>X<h>" and surrounding spaces. Returns false
// on anything it does not fully understand -- a half-parsed size would silently
// build the wrong map, which is worse than ignoring the line.
static bool ParseWxH(const char* s, int* w, int* h)
{
    if (!s || !*s) return false;
    char* end = nullptr;
    long a = strtol(s, &end, 10);
    if (end == s || a <= 0) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != 'x' && *end != 'X') return false;
    ++end;
    const char* p2 = end;
    long b = strtol(p2, &end, 10);
    if (end == p2 || b <= 0) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end) return false;                 // trailing junk: refuse
    *w = (int)a; *h = (int)b;
    return true;
}
// The engine cannot survive its own 224 clamp with the stock street raster.
//
// "Creating streets" builds a 1-metre occupancy raster over the whole map
// bounding box (RVA 0x90d410) and sizes it with a 32-bit signed multiply:
//
//     mov    eax, [rbx+0x44]          ; ny
//     imul   eax, dword [rbx+0x40]    ; nx * ny   <-- 32-bit
//     movsxd rdx, eax                 ; sign-extend into size_t
//     call   vector<bool>::resize
//
// At 224 tiles the map is 56000 m per side, so nx*ny = 56001^2 =
// 3,136,112,001 > INT_MAX. It wraps to -1,158,855,295, sign-extends to ~1.8e19,
// and vector<bool>::resize throws std::length_error. Nothing catches it:
// std::terminate -> abort -> SIGABRT with no message (it is an uncaught C++
// exception, not an assert, which is why the game's assert handler prints
// nothing). Confirmed by resolving the thrown object's RTTI in the minidump:
// .?AVlength_error@std@@
//
// The rule is (width_m + 1) * (height_m + 1) <= 2147483647, i.e. <= 46339 m on a
// square map. A tile is 256 m, so 180 tiles = 46080 m -> 46081^2 =
// 2,123,458,561 fits and 182 tiles = 46592 m -> 2,170,907,649 does not.
// 180 is therefore the stock ceiling -- but street_raster=1 lifts it.
//
// Non-square maps get more in one axis under the same product rule: 300 x 114
// tiles (75 x 28.5 km) is legal.
static int g_maxTiles    = 184;
static int g_logEvery    = 1;

// The real per-axis ceiling is the OCTREE root box, not a square-derived
// scalar: an entity past the box stacks duplicate nodes (see the octree
// notes above). Stock holds +-32,768 m = 256 tiles; octree=1 widens it to
// +-65,536 m = 512 tiles (depth 12: 1024; depth 13: 2048).
// A rectangle's long axis may use all of it -- its
// area (the street-raster product, handled by the raster hook / warned
// below) is a separate limit. max_tiles stays an explicit lower cap.
static int EffectiveMaxTiles()
{
    int octCap = g_octreeOn ? (g_octreeDepth == 13 ? 2048 :
                              g_octreeDepth == 12 ? 1024 : OCTREE_PATCH_TILES)
                            : OCTREE_STOCK_TILES;
    return (g_maxTiles > 0 && g_maxTiles < octCap) ? g_maxTiles : octCap;
}

// Even, and inside [2, max]. The engine's own override path asserts on
// `worldDimensions.x % 2 == 0`, so something downstream assumes it; we are past
// that assert here, which makes honouring the rule our job rather than the
// engine's.
static int Sanitise(int tiles)
{
    int cap = EffectiveMaxTiles();
    if (tiles < 2) tiles = 2;
    if (tiles > cap) tiles = cap;
    if (tiles & 1) tiles -= 1;
    return tiles;
}

static bool HeightmapFits(int tx, int ty)
{
    return (int64_t(tx) * 64 + 1) * (int64_t(ty) * 64 + 1) <= 2147483647LL;
}

// The new edge cap permits rectangles whose heightmap exceeds INT_MAX.
// Keep explicit cells even and reduce the longer axis until the product fits.
static void BoundHeightmap(int* tx, int* ty)
{
    while (!HeightmapFits(*tx, *ty)) {
        if (*tx >= *ty) *tx -= 2;
        else *ty -= 2;
    }
}

extern "C" __declspec(dllexport)
void BigmapTestOctreeSize(int depth, int maxTiles, int enabled, int* tx, int* ty)
{
    int oldDepth = g_octreeDepth, oldMax = g_maxTiles;
    bool oldOn = g_octreeOn;
    g_octreeDepth = depth; g_maxTiles = maxTiles; g_octreeOn = enabled != 0;
    *tx = Sanitise(*tx); *ty = Sanitise(*ty);
    BoundHeightmap(tx, ty);
    g_octreeDepth = oldDepth; g_maxTiles = oldMax; g_octreeOn = oldOn;
}

// A std::string holding `s` in its inline buffer (SSO: at most 15 chars).
static void SsoString(MsvcString* out, const char* s)
{
    memset(out, 0, sizeof *out);
    size_t n = strlen(s);
    if (n > 15) n = 15;
    memcpy(out->buf, s, n);
    out->size = n;
    out->cap  = 15;
}

// Grow a vector<string> the ENGINE owns by `k` strings without our allocator
// ever touching it: construct a new vector from (its strings + extra) with the
// engine's range constructor, swap it in, destroy the old one with the engine's
// destructor. `src` is only a byte view of the elements -- the constructor
// deep-copies each one, SSO or heap -- and nothing in it is ever destroyed, so
// nothing is freed twice.
static bool AppendStrings(StrVec* vec, const MsvcString* extra, int k)
{
    const size_t n = (size_t)(vec->last - vec->first);
    MsvcString* src = (MsvcString*)malloc((n + (size_t)k) * sizeof(MsvcString));
    if (!src) return false;
    if (n) memcpy(src, vec->first, n * sizeof(MsvcString));
    memcpy(src + n, extra, (size_t)k * sizeof(MsvcString));
    StrVec fresh = { nullptr, nullptr, nullptr };
    g_strVecCtor(&fresh, src, src + n + (size_t)k, nullptr);
    free(src);
    StrVec old = *vec;
    *vec = fresh;
    g_strVecDtor(&old);
    return true;
}

// Combo factory: for the New Game size dropdown only, note how many stock rows
// it has and append ours before the widget copies the list.
static void* __fastcall ComboDetour(StrVec* items, void* onChange, int initialSel, void* aux, int p5)
{
    if ((uintptr_t)_ReturnAddress() == g_base + RET_SIZE_COMBO && items) {
        const long stock = (long)(items->last - items->first);
        if (stock > 0 && stock < 64) {
            InterlockedExchange(&g_stockSizeRows, stock);
            const bool ok = AppendStrings(items, g_extraRowStrings, g_numExtraSizeRows);
            if (g_logEvery)
                H->log("size dropdown: %ld stock row(s) + %d added%s", stock,
                       ok ? g_numExtraSizeRows : 0, ok ? "" : " (append FAILED)");
        }
    }
    return g_origCombo(items, onChange, initialSel, aux, p5);
}

// The claim for (size, format), else that size's lowest-format claim; -1 when
// the size has none.
static int FindClaim(int size, int format)
{
    int fallback = -1;
    for (int i = 0; i < g_numClaims; ++i) {
        if (g_claims[i].size != size) continue;
        if (g_claims[i].format == format) return i;
        if (fallback < 0 || g_claims[i].format < g_claims[fallback].format) fallback = i;
    }
    return fallback;
}

// The claim for exactly (size, format); -1 when that cell was not set.
static int FindClaimExact(int size, int format)
{
    for (int i = 0; i < g_numClaims; ++i)
        if (g_claims[i].size == size && g_claims[i].format == format) return i;
    return -1;
}

// A ratio cell nobody set, shaped from the row's square the way the game's own
// preset table is laid out (0x140881070: Megalomaniac 96x96, 66x132, 54x162,
// 48x192; Medium 44x44, 32x64, 26x78, 22x88, 20x100): 1:k keeps about the
// square's area -- short side = side / sqrt(k) to the nearest even tile count,
// long side = k times that. A long side past `cap` is brought back to it with
// the exact ratio kept, so the biggest rows' narrow shapes come out smaller.
static void DeriveShape(int side, int format, int cap, int* tx, int* ty)
{
    // Bound even malformed/stale menu indices before arithmetic. Tiny custom
    // caps cannot fit a two-tile-wide strip at every ratio.
    if(cap<2)cap=2;
    int k = (format < 0 ? 0 : format >= MAX_RATIOS ? MAX_RATIOS-1 : format) + 1;
    if(k>cap/2)k=cap/2;
    int x = 2 * (int)std::floor(side / std::sqrt((double)k) / 2.0 + 0.5);
    if (x < 2) x = 2;
    if (x * k > cap) x = 2 * ((cap / k) / 2);
    if (x < 2) x = 2;
    *tx = x;
    *ty = x * k;
    while (x > 2 && !HeightmapFits(*tx, *ty)) {
        x -= 2;
        *tx = x; *ty = x * k;
    }
}

// Offline test entry (tools/test_newgame_menu.py).
extern "C" __declspec(dllexport)
void BigmapTestDeriveShape(int side, int format, int cap, int* tx, int* ty)
{
    DeriveShape(side, format, cap, tx, ty);
}

// The shape an added row gives for one ratio: the cell set in the cfg if there
// is one, else derived from the row's square (its lowest-format claim).
static bool RowShape(int size, int format, int* tx, int* ty)
{
    const int exact = FindClaimExact(size, format);
    if (exact >= 0) {
        *tx = g_claims[exact].tx;
        *ty = g_claims[exact].ty;
        return true;
    }
    const int base = FindClaim(size, 0);
    int side = 96;
    if (base >= 0) {
        const Claim& c = g_claims[base];
        side = (c.tx == c.ty) ? c.tx : 2 * (int)(std::sqrt((double)c.tx * c.ty) / 2.0);
    }
    DeriveShape(side, format, EffectiveMaxTiles(), tx, ty);
    return false;
}

// ---------------------------------------------------------------------------
static uint64_t __fastcall Detour(int sizeIndex, int formatIndex, void* cfg)
{
    if(sizeIndex<0)sizeIndex=0;
    if(formatIndex<0 || formatIndex>=MAX_RATIOS)formatIndex=0;
    // An ADDED size row first. Its raw index is past the stock rows the combo
    // was built with (4 with experimental map sizes off, 7 with it on), and with
    // the flag off that overlaps what the ladder calls size 4..6 -- so it has to
    // be recognised before any claim is consulted.
    if (g_sizeRowsLive) {
        const int row = sizeIndex - (int)g_stockSizeRows;
        if (row >= 0 && row < g_numExtraSizeRows) {
            int tx = 96, ty = 96;
            const bool claimed = RowShape(g_extraSizeRows[row].size, formatIndex, &tx, &ty);
            if (g_logEvery) {
                H->log("size=%d format=%d -> %d x %d tiles (%.1f x %.1f km) [added row '%s', %s]",
                       sizeIndex, formatIndex, tx, ty, tx * 0.256, ty * 0.256,
                       g_extraSizeRows[row].label, claimed ? "cfg cell" : "ratio shape");
            }
            return ((uint64_t)(uint32_t)ty << 32) | (uint32_t)tx;
        }
    }
    // The ladder is checked next: an explicit size<S>_format<F> entry is a
    // deliberate statement about one cell and should beat the older single
    // tiles_x/tiles_y pair, which is kept only so existing configs keep working.
    for (int i = 0; i < g_numClaims; ++i) {
        if (g_claims[i].size == sizeIndex && g_claims[i].format == formatIndex) {
            int tx = g_claims[i].tx, ty = g_claims[i].ty;
            uint64_t packed = ((uint64_t)(uint32_t)ty << 32) | (uint32_t)tx;
            if (g_logEvery) {
                H->log("size=%d format=%d -> %d x %d tiles (%.1f x %.1f km) [ladder]",
                       sizeIndex, formatIndex, tx, ty, tx * 0.256, ty * 0.256);
            }
            return packed;
        }
    }
    if (sizeIndex == g_sizeIndex && formatIndex == g_formatIndex
        && g_tilesX > 0 && g_tilesY > 0) {
        uint64_t packed = ((uint64_t)(uint32_t)g_tilesY << 32) | (uint32_t)g_tilesX;
        if (g_logEvery) {
            H->log("size=%d format=%d -> %d x %d tiles (%.1f x %.1f km)",
                   sizeIndex, formatIndex, g_tilesX, g_tilesY,
                   g_tilesX * 0.256, g_tilesY * 0.256);
        }
        return packed;
    }
    // Nothing past the stock presets may reach g_orig: it asserts sizeIndex < 7.
    // Every added row is answered above, so this is not expected; answer the
    // closest claim (or the stock Megalomaniac square) and say so, not crash.
    if (sizeIndex >= 7) {
        const int ci = FindClaim(sizeIndex, formatIndex);
        const int tx = ci >= 0 ? g_claims[ci].tx : 96;
        const int ty = ci >= 0 ? g_claims[ci].ty : 96;
        H->log("size=%d format=%d -> %d x %d tiles [past the stock presets%s]", sizeIndex,
               formatIndex, tx, ty, ci >= 0 ? "" : ", no claim: DEFAULT");
        return ((uint64_t)(uint32_t)ty << 32) | (uint32_t)tx;
    }
    // Stock rows also support added ratios. Query only their legal square
    // preset, retaining the engine's experimental-size index translation.
    // Explicit world-dimension overrides retain the stock override behavior.
    if(formatIndex>=5) {
        if(FindClaim(sizeIndex,0)>=0) {
            int tx,ty;RowShape(sizeIndex,formatIndex,&tx,&ty);
            return (uint64_t(uint32_t(ty))<<32)|uint32_t(tx);
        }
        uint64_t square=g_orig(sizeIndex,0,cfg);
        if(cfg && *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(cfg)+0x28)>0 &&
                  *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(cfg)+0x2c)>0)return square;
        int sx=int(uint32_t(square)),sy=int(uint32_t(square>>32));
        int side=int(std::sqrt(double(sx)*sy));
        int tx,ty;DeriveShape(side,formatIndex,EffectiveMaxTiles(),&tx,&ty);
        return (uint64_t(uint32_t(ty))<<32)|uint32_t(tx);
    }
    // Not ours: the stock size, computed by the game's own code. Every preset
    // keeps working, including the ones we did not claim.
    return g_orig(sizeIndex, formatIndex, cfg);
}

#include "map_ratios.h"

// Number of cells the ctor will produce for a given cell size, using the
// engine's own rounding: n = floor(extent / cell) + 1 per axis.
static double CellsFor(float w, float h, float cell)
{
    double nx = (double)(int)(w / cell) + 1.0;
    double ny = (double)(int)(h / cell) + 1.0;
    return nx * ny;
}

static void* __fastcall RasterDetour(void* self, const float* bbox, float cellSize)
{
    float cell = cellSize;
    if (!(cell > 0.0f)) cell = 1.0f;            // also catches NaN
    float w = bbox[2] - bbox[0];
    float h = bbox[3] - bbox[1];
    if (!(w > 0.0f) || !(h > 0.0f))
        return g_origRaster(self, bbox, cellSize);   // degenerate: leave alone

    double cells = CellsFor(w, h, cell);
    if (cells > g_cellBudget) {
        float grown = cell;
        // Integer cell sizes only -- a fractional grid buys nothing and makes
        // the raster harder to reason about. 64 is a sanity stop, not a limit
        // we expect to reach (it would be a ~3000 km map).
        for (int i = 0; i < 64 && CellsFor(w, h, grown) > g_cellBudget; ++i)
            grown += 1.0f;
        H->log("street raster: %.0f x %.0f m at %.0f m/cell = %.3fe9 cells "
               "(over budget) -> %.0f m/cell = %.3fe9 cells, %.0f MB",
               w, h, cell, cells / 1e9, grown,
               CellsFor(w, h, grown) / 1e9, CellsFor(w, h, grown) / 8.0 / 1e6);
        cell = grown;
    }
    return g_origRaster(self, bbox, cell);
}

// ---------------------------------------------------------------------------
// base_mod.lua: the density levels in the stock lists, and industryFreq
// ---------------------------------------------------------------------------
// The Towns, Number of industries and Industry density target dropdowns list
// base_mod.lua's own param values, so the extra levels are added to those lists
// in res/config/base_mod.lua, and runFn's industryFreq gets one multiplier per
// level. That is a game file, so it is PATCHED, never replaced with a frozen
// copy: on every start the plugin takes the stock text (the file itself when it
// carries no patch, else the backup it kept as base_mod.lua.bigmapbak), applies
// anchored inserts (each anchor must match exactly once, in order) and writes
// only when the result differs. A game update or Steam's "verify integrity" is
// re-patched on the next start, and a changed file whose anchors no longer
// match is left untouched, levels OFF.
//
// runFn runs on LOAD as well as at generation, with the params stored in the
// save, so a map made with an extra industry level needs the patched runFn
// wherever it is loaded: the stock one has no multiplier for that index. When
// the levels cannot go live (GOG, a byte mismatch, newgame_density=0) the stock
// file is put back, so the page never offers a level the game cannot apply.
static const char BASEMOD_ANCHOR_HEAD[] = "local osutil = require \"osutil\"\n";
static const char BASEMOD_INSERT_HEAD[] =
    "\n"
    "-- tpf2_bigmap: extra density levels at the end of the Towns, Number of industries\n"
    "-- and Industry density target lists, and their multipliers in runFn's industryFreq.\n"
    "-- Added by plugins\\tpf2_bigmap.dll, which re-applies it on every start and keeps\n"
    "-- the stock file beside this one as base_mod.lua.bigmapbak.\n";

// The PC (gen 3) lists; gens 1 and 2 are the consoles'. Each anchor ends right
// before its list's closing brace, and only the gen 3 lines hold "Very high".
static const char BASEMOD_ANCHOR_TOWNS[] =
    "pGetText(\"map-town-density\", \"High\"), pGetText(\"map-town-density\", \"Very high\"), ";
static const char BASEMOD_ANCHOR_INDUSTRIES[] =
    "{ pGetText(\"map-industry-density\", \"Low\"), pGetText(\"map-industry-density\", \"Medium\"), "
    "pGetText(\"map-industry-density\", \"High\"), pGetText(\"map-industry-density\", \"Very high\"), ";
static const char BASEMOD_ANCHOR_TARGET[] =
    "{ pGetText(\"map-industry-density\", \"Disabled\"), pGetText(\"map-industry-density\", \"Low\"), "
    "pGetText(\"map-industry-density\", \"Medium\"), pGetText(\"map-industry-density\", \"High\"), "
    "pGetText(\"map-industry-density\", \"Very high\"), ";
// The page stores the industries index as both start (idx) and target (idx + 1,
// past "Disabled"), and runFn reads industryFreq[idx + 1] and industryFreq[target],
// so one appended multiplier per level serves both lists.
static const char BASEMOD_ANCHOR_FREQ[] = "local industryFreq = { .4, .6, .8, 1.0";
static const char BASEMOD_ANCHOR_IDX[] =
    "local targetNumberPerAreaIdx = modParams[\"locations.industry.targetMaxNumberPerArea\"]\n";
static const char BASEMOD_INSERT_IDX[] =
    "\t\tif type(startIndustriesIdx) == \"number\" and startIndustriesIdx > 3 then\n"
    "\t\t\tprint(string.format(\"[tpf2_bigmap] industry density level %d: x%g\", startIndustriesIdx,\n"
    "\t\t\t\tindustryFreq[startIndustriesIdx + 1] or -1))\n"
    "\t\tend\n";

// Built from DENSITY_LEVELS on first use: the labels, and the industryFreq tail.
static char g_insertLabels[512];
static char g_insertFreq[160];

static void BuildBaseModInserts()
{
    if (g_insertLabels[0]) return;
    size_t o = 0;
    for (int i = 0; i < NUM_DENSITY_LEVELS; ++i) {
        const int n = _snprintf_s(g_insertLabels + o, sizeof g_insertLabels - o, _TRUNCATE,
                                  "_(\"%s\"), ", DENSITY_LEVELS[i].label);
        if (n < 0) break;
        o += (size_t)n;
    }
    o = 0;
    for (int i = 0; i < NUM_DENSITY_LEVELS; ++i) {
        const int n = _snprintf_s(g_insertFreq + o, sizeof g_insertFreq - o, _TRUNCATE,
                                  ", %.6g", INDUSTRY_MEDIUM * DENSITY_LEVELS[i].scale);
        if (n < 0) break;
        o += (size_t)n;
    }
}

enum {
    BASEMOD_ALREADY     =  0,   // already the patched stock text; nothing written
    BASEMOD_PATCHED     =  1,   // patched now (the stock backup written first)
    BASEMOD_STOCK       =  2,   // stock wanted and it already is; nothing written
    BASEMOD_RESTORED    =  3,   // a patch was removed: stock put back from the backup
    BASEMOD_ERR_READ    = -1,   // base_mod.lua could not be read
    BASEMOD_ERR_FOREIGN = -2,   // carries a patch and there is no clean backup
    BASEMOD_ERR_ANCHOR  = -3,   // an anchor is missing or repeated: the game changed the file
    BASEMOD_ERR_WRITE   = -4,   // a write failed
};

static const char* BaseModResultText(int rc)
{
    switch (rc) {
    case BASEMOD_ALREADY:     return "density levels already in place";
    case BASEMOD_PATCHED:     return "density levels added (stock kept as base_mod.lua.bigmapbak)";
    case BASEMOD_STOCK:       return "stock, left untouched";
    case BASEMOD_RESTORED:    return "stock file restored from base_mod.lua.bigmapbak";
    case BASEMOD_ERR_READ:    return "NOT patched: unreadable";
    case BASEMOD_ERR_FOREIGN: return "NOT patched: carries a patch with no clean backup";
    case BASEMOD_ERR_ANCHOR:  return "NOT patched: the game's file changed shape";
    case BASEMOD_ERR_WRITE:   return "NOT patched: write failed";
    }
    return "?";
}

static void Why(char* why, size_t len, const char* fmt, ...)
{
    if (!why || !len) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(why, len, _TRUNCATE, fmt, ap);
    va_end(ap);
}

// The whole file, NUL-terminated, in a malloc'd buffer.
static bool ReadWholeFile(const wchar_t* path, char** data, size_t* len)
{
    *data = nullptr;
    *len = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (16LL << 20);
    char* buf = ok ? (char*)malloc((size_t)size.QuadPart + 1) : nullptr;
    DWORD got = 0;
    ok = buf && ReadFile(f, buf, (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart;
    DWORD err = GetLastError();
    CloseHandle(f);
    if (!ok) {
        free(buf);
        SetLastError(err);
        return false;
    }
    buf[got] = 0;
    *data = buf;
    *len = got;
    return true;
}

// Replace `path` through a temp file in the same folder, so a crash or a full
// disk mid-write never leaves a truncated base_mod.lua behind.
static bool WriteWholeFile(const wchar_t* path, const char* data, size_t len)
{
    wchar_t tmp[MAX_PATH];
    if (_snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.bigmaptmp", path) < 0) return false;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, (DWORD)len, &put, nullptr) && put == (DWORD)len && FlushFileBuffers(f);
    DWORD err = GetLastError();
    CloseHandle(f);
    if (ok) {
        ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        err = GetLastError();
    }
    if (!ok) {
        DeleteFileW(tmp);
        SetLastError(err);
    }
    return ok;
}

// The in-game minimap: native terrain texture behind a Lua ImageView. Uses
// ReadWholeFile / WriteWholeFile above for its game script.
#include "minimap.h"

// The stock text with the inserts, malloc'd; nullptr (and `why`) when an anchor
// is missing, repeated or out of order.
static char* PatchBaseModText(const char* stock, size_t len, size_t* outLen, char* why, size_t whyLen)
{
    BuildBaseModInserts();
    static const struct { const char* name; const char* anchor; const char* insert; } steps[] = {
        { "require \"osutil\"",             BASEMOD_ANCHOR_HEAD,       BASEMOD_INSERT_HEAD },
        { "Towns list",                     BASEMOD_ANCHOR_TOWNS,      g_insertLabels },
        { "Number of industries list",      BASEMOD_ANCHOR_INDUSTRIES, g_insertLabels },
        { "Industry density target list",   BASEMOD_ANCHOR_TARGET,     g_insertLabels },
        { "industryFreq",                   BASEMOD_ANCHOR_FREQ,       g_insertFreq },
        { "targetNumberPerAreaIdx",         BASEMOD_ANCHOR_IDX,        BASEMOD_INSERT_IDX },
    };
    const int nsteps = (int)(sizeof steps / sizeof steps[0]);
    size_t at[sizeof steps / sizeof steps[0]];
    size_t total = len;
    for (int i = 0; i < nsteps; ++i) {
        const char* hit = strstr(stock, steps[i].anchor);
        const char* again = hit ? strstr(hit + 1, steps[i].anchor) : nullptr;
        if (!hit || again) {
            Why(why, whyLen, "anchor '%s' found %s%s", steps[i].name,
                hit ? "more than once" : "nowhere",
                strstr(stock, "\r\n") ? " (the file has CRLF line endings; the stock file is LF)" : "");
            return nullptr;
        }
        at[i] = (size_t)(hit - stock) + strlen(steps[i].anchor);
        if (i > 0 && at[i] <= at[i - 1]) {
            Why(why, whyLen, "anchor '%s' is out of order", steps[i].name);
            return nullptr;
        }
        total += strlen(steps[i].insert);
    }
    char* out = (char*)malloc(total + 1);
    if (!out) {
        Why(why, whyLen, "out of memory");
        return nullptr;
    }
    size_t from = 0, o = 0;
    for (int i = 0; i < nsteps; ++i) {
        memcpy(out + o, stock + from, at[i] - from);
        o += at[i] - from;
        from = at[i];
        const size_t n = strlen(steps[i].insert);
        memcpy(out + o, steps[i].insert, n);
        o += n;
    }
    memcpy(out + o, stock + from, len - from);
    o += len - from;
    out[o] = 0;
    *outLen = o;
    return out;
}

// Make `path` the patched stock text (want) or the stock text (!want). The stock
// file never contains "bigmap"; our patch and the earlier hand-made test patch
// both do, which is how a patched file is told apart.
static int SyncBaseMod(const wchar_t* path, bool want, char* why, size_t whyLen)
{
    if (why && whyLen) why[0] = 0;
    wchar_t bak[MAX_PATH];
    if (_snwprintf_s(bak, MAX_PATH, _TRUNCATE, L"%s.bigmapbak", path) < 0) {
        Why(why, whyLen, "path too long");
        return BASEMOD_ERR_READ;
    }
    char* cur = nullptr;
    size_t curLen = 0;
    if (!ReadWholeFile(path, &cur, &curLen)) {
        Why(why, whyLen, "cannot read it (error %lu)", GetLastError());
        return BASEMOD_ERR_READ;
    }
    const bool curPatched = strstr(cur, "bigmap") != nullptr;
    char* bakText = nullptr;
    size_t bakLen = 0;
    const char* stock = cur;
    size_t stockLen = curLen;
    if (curPatched) {
        if (!ReadWholeFile(bak, &bakText, &bakLen) || strstr(bakText, "bigmap")) {
            free(cur);
            free(bakText);
            Why(why, whyLen, "base_mod.lua.bigmapbak is missing or patched too; Steam's "
                "\"Verify integrity of game files\" restores the stock file");
            return BASEMOD_ERR_FOREIGN;
        }
        stock = bakText;
        stockLen = bakLen;
    }
    int rc;
    if (!want) {
        rc = !curPatched ? BASEMOD_STOCK
           : WriteWholeFile(path, stock, stockLen) ? BASEMOD_RESTORED : BASEMOD_ERR_WRITE;
    } else {
        size_t outLen = 0;
        char* patched = PatchBaseModText(stock, stockLen, &outLen, why, whyLen);
        if (!patched)
            rc = BASEMOD_ERR_ANCHOR;
        else if (curPatched && outLen == curLen && memcmp(patched, cur, curLen) == 0)
            rc = BASEMOD_ALREADY;
        else if (!curPatched && !WriteWholeFile(bak, cur, curLen))   // keep the stock text first
            rc = BASEMOD_ERR_WRITE;
        else
            rc = WriteWholeFile(path, patched, outLen) ? BASEMOD_PATCHED : BASEMOD_ERR_WRITE;
        free(patched);
    }
    if (rc == BASEMOD_ERR_WRITE) Why(why, whyLen, "cannot write it (error %lu)", GetLastError());
    free(cur);
    free(bakText);
    return rc;
}

// <game>\res\config\base_mod.lua, from the running exe's own path.
static bool GameBaseModPath(wchar_t* out, size_t cch)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    *slash = 0;
    return _snwprintf_s(out, cch, _TRUNCATE, L"%s\\res\\config\\base_mod.lua", exe) > 0;
}

static int SyncGameBaseMod(bool want)
{
    wchar_t path[MAX_PATH];
    if (!GameBaseModPath(path, MAX_PATH)) {
        H->log("base_mod.lua: cannot work out the game folder -- left alone");
        return BASEMOD_ERR_READ;
    }
    char why[320];
    const int rc = SyncBaseMod(path, want, why, sizeof why);
    H->log("base_mod.lua: %s%s%s", BaseModResultText(rc), why[0] ? " -- " : "", why);
    return rc;
}

// Offline test entry (tools/test_newgame_menu.py): the same sync on any file.
// The plugin host only ever calls Tpf2mpPluginInit; nothing in the game calls this.
extern "C" __declspec(dllexport)
int BigmapTestSyncBaseMod(const wchar_t* path, int want, char* why, int whyLen)
{
    return SyncBaseMod(path, want != 0, why, whyLen > 0 ? (size_t)whyLen : 0);
}

// Uninstall entry: rundll32 "<game>\plugins\tpf2_bigmap.dll",BigmapRestoreStockBaseMod
// (installer\Package.wxs runs it before the files go). Puts the stock base_mod.lua
// back from the backup next to it; a stock or missing file is left alone. The game
// folder is two levels above this DLL, so a copy elsewhere (the datadir plugins
// folder) finds no file and does nothing.
extern "C" __declspec(dllexport)
void WINAPI BigmapRestoreStockBaseMod(HWND, HINSTANCE, LPSTR, int)
{
    wchar_t self[MAX_PATH];
    HMODULE me = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&BigmapRestoreStockBaseMod, &me)) return;
    DWORD n = GetModuleFileNameW(me, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    for (int up = 0; up < 2; ++up) {              // strip "\tpf2_bigmap.dll", then "\plugins"
        wchar_t* slash = wcsrchr(self, L'\\');
        if (!slash) return;
        *slash = 0;
    }
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\res\\config\\base_mod.lua", self) < 0) return;
    char why[320];
    SyncBaseMod(path, false, why, sizeof why);
    // The minimap game script goes too (only if it is ours).
    wchar_t script[MAX_PATH];
    if (MinimapScriptPathIn(self, script, MAX_PATH)) SyncMinimapScript(script, false);
}

// ---------------------------------------------------------------------------
// Installing the New Game rows
// ---------------------------------------------------------------------------
static bool g_vecHelpersOk = false;

static bool ResolveVectorHelpers()
{
    if (g_vecHelpersOk) return true;
    if (!H->verifyBytes(RVA_STRVEC_CTOR, EXPECT_STRVEC_CTOR, sizeof EXPECT_STRVEC_CTOR)
        || !H->verifyBytes(RVA_STRVEC_DTOR, EXPECT_STRVEC_DTOR, sizeof EXPECT_STRVEC_DTOR)) {
        H->log("new game rows: vector<string> ctor/dtor do not byte-verify -- no rows added");
        return false;
    }
    g_strVecCtor = (StrVecCtorFn)(g_base + RVA_STRVEC_CTOR);
    g_strVecDtor = (StrVecDtorFn)(g_base + RVA_STRVEC_DTOR);
    g_vecHelpersOk = true;
    return true;
}

static void FillTownMultipliers()
{
    const int cap = (int)(sizeof g_townMult / sizeof g_townMult[0]);
    for (int i = 0; i < NUM_DENSITY_LEVELS && i < cap; ++i)
        g_townMult[i] = (float)(TOWN_MEDIUM * DENSITY_LEVELS[i].scale);
}

// The Towns switch stub. Entered by an absolute jmp from 0x14065b711 with the
// Towns index in ecx; leaves the multiplier in xmm0 and jumps to the kept store
// at 0x14065b728, where the stock 0.2/0.3/0.4 cases land too. rax and rcx are
// free there: both are reloaded before their next read.
static void* BuildTownStub(uintptr_t back, const float* table, int n)
{
    static const uint8_t tmpl[66] = {
        0x89, 0xC8,                               //  0  mov   eax, ecx
        0x83, 0xF8, 0x03,                         //  2  cmp   eax, 3
        0x75, 0x0B,                               //  5  jne   18
        0xB9, 0x00, 0x00, 0x00, 0x3F,             //  7  mov   ecx, 0.5f
        0x66, 0x0F, 0x6E, 0xC1,                   // 12  movd  xmm0, ecx
        0xEB, 0x22,                               // 16  jmp   52
        0x83, 0xE8, 0x04,                         // 18  sub   eax, 4
        0x83, 0xF8, 0x00,                         // 21  cmp   eax, n              (imm8 at 23)
        0x73, 0x11,                               // 24  jae   43
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,       // 26  mov   rcx, table          (imm64 at 28)
        0xF3, 0x0F, 0x10, 0x04, 0x81,             // 36  movss xmm0, [rcx + rax*4]
        0xEB, 0x09,                               // 41  jmp   52
        0xB9, 0x00, 0x00, 0x80, 0x3F,             // 43  mov   ecx, 1.0f
        0x66, 0x0F, 0x6E, 0xC1,                   // 48  movd  xmm0, ecx
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,       // 52  jmp   [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0,                   // 58  back                     (abs64)
    };
    if (n < 0 || n > 127) return nullptr;
    uint8_t code[sizeof tmpl];
    memcpy(code, tmpl, sizeof code);
    code[23] = (uint8_t)n;
    const uint64_t tableAddr = (uint64_t)(uintptr_t)table;
    const uint64_t backAddr = (uint64_t)back;
    memcpy(code + 28, &tableAddr, 8);
    memcpy(code + 58, &backAddr, 8);
    void* mem = VirtualAlloc(nullptr, sizeof code, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return nullptr;
    memcpy(mem, code, sizeof code);
    DWORD old = 0;
    if (!VirtualProtect(mem, sizeof code, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof code);
    return mem;
}

// Offline test entry (tools/test_newgame_menu.py): the Towns stub returning to
// `back` instead of into the game, so a test can call it as float(int).
extern "C" __declspec(dllexport)
void* BigmapTestBuildTownStub(void* back)
{
    FillTownMultipliers();
    return BuildTownStub((uintptr_t)back, g_townMult, NUM_DENSITY_LEVELS);
}

// Density levels. Independent of the size config, so it runs straight after the
// build is known. Returns the number of features made live (0 or 1).
static int InstallDensityLevels()
{
    if (!g_newgameDensity) {
        SyncGameBaseMod(false);
        H->log("density levels: off (newgame_density=0)");
        return 0;
    }
    if (g_gog) {
        SyncGameBaseMod(false);
        H->log("density levels: Steam build only -- not on the GOG build");
        return 0;
    }
    if (!H->verifyBytes(RVA_TOWN_SWITCH_HEAD, EXPECT_TOWN_SWITCH_HEAD, sizeof EXPECT_TOWN_SWITCH_HEAD)
        || !H->verifyBytes(RVA_TOWN_SWITCH_TAIL, EXPECT_TOWN_SWITCH_TAIL, sizeof EXPECT_TOWN_SWITCH_TAIL)) {
        SyncGameBaseMod(false);
        H->log("density levels: the Towns switch does not byte-verify -- OFF");
        return 0;
    }
    // Labels first, then the stub: if the stub cannot go in, the labels come back
    // out, because a Towns level past Very high with no stub reads as x1.0.
    const int rc = SyncGameBaseMod(true);
    if (rc != BASEMOD_ALREADY && rc != BASEMOD_PATCHED) {
        H->log("density levels: OFF -- base_mod.lua could not be patched");
        return 0;
    }
    FillTownMultipliers();
    void* stub = BuildTownStub(g_base + RVA_TOWN_SWITCH_STORE, g_townMult, NUM_DENSITY_LEVELS);
    uint8_t jmp[23];
    memset(jmp, 0x90, sizeof jmp);                // NOPs after the jmp
    jmp[0] = 0xFF; jmp[1] = 0x25;                 // jmp [rip+0]
    jmp[2] = jmp[3] = jmp[4] = jmp[5] = 0x00;
    const uint64_t stubAddr = (uint64_t)(uintptr_t)stub;
    memcpy(jmp + 6, &stubAddr, 8);
    if (!stub || !H->patchBytes(RVA_TOWN_SWITCH_TAIL, jmp, sizeof jmp)) {
        SyncGameBaseMod(false);
        H->log("density levels: could not redirect the Towns switch -- OFF");
        return 0;
    }
    H->log("density levels: live -- %d levels after Very high in Towns, Number of industries "
           "and Industry density target (Towns switch -> stub at %p)", NUM_DENSITY_LEVELS, stub);
    return 1;
}

// Size rows. Needs the GetNumTilesNew detour, so it runs after that is in.
static int InstallSizeRows()
{
    if (!g_addSizeRows) return 0;
    if (g_numExtraSizeRows <= 0) {
        H->log("size rows: add_size_rows=1 but no size7_format0 claim -- nothing to add");
        return 0;
    }
    if (g_gog) {
        H->log("size rows: Steam build only -- not on the GOG build");
        return 0;
    }
    if (!ResolveVectorHelpers()
        || !H->verifyBytes(RVA_COMBO_FAC, EXPECT_COMBO_FAC, sizeof EXPECT_COMBO_FAC)
        || !H->verifyBytes(RET_SIZE_COMBO - 5, CALL_SIZE_COMBO, sizeof CALL_SIZE_COMBO)) {
        H->log("size rows: the size dropdown code does not byte-verify -- OFF");
        return 0;
    }
    for (int i = 0; i < g_numExtraSizeRows; ++i)
        SsoString(&g_extraRowStrings[i], g_extraSizeRows[i].label);
    void* t = nullptr;
    if (!H->installHook(g_base + RVA_COMBO_FAC, (void*)&ComboDetour, sizeof EXPECT_COMBO_FAC, &t)) {
        H->log("size rows: installHook failed on the combo factory -- OFF");
        return 0;
    }
    g_origCombo = (ComboFacFn)t;
    g_sizeRowsLive = true;
    H->log("size rows: live -- %d row(s) after the stock sizes, with experimental map "
           "sizes on or off", g_numExtraSizeRows);
    return 1;
}

// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport)
int Tpf2mpPluginInit(const Tpf2mpHost* host, Tpf2mpPluginInfo* out)
{
    out->name    = "bigmap";
    out->version = "0.2";
    out->summary = "larger maps than the New Game menu offers (Steam build 35924)";

    if (!host || host->abiMajor != TPF2MP_ABI_MAJOR
        || host->size < sizeof(Tpf2mpHost)) return TPF2MP_ERR_ABI;
    H = host;

    g_tilesX      = H->cfgInt ("tpf2_bigmap", "tiles_x",      0);
    g_tilesY      = H->cfgInt ("tpf2_bigmap", "tiles_y",      0);
    g_sizeIndex   = H->cfgInt ("tpf2_bigmap", "size_index",   6);
    g_formatIndex = H->cfgInt ("tpf2_bigmap", "format_index", 0);
    g_maxTiles    = H->cfgInt ("tpf2_bigmap", "max_tiles",    224);
    g_maxRatio = H->cfgInt("tpf2_bigmap", "max_ratio", MAX_RATIOS);
    if(g_maxRatio<5 || g_maxRatio>MAX_RATIOS)g_maxRatio=MAX_RATIOS;
    g_logEvery    = H->cfgBool("tpf2_bigmap", "log",          1);
    g_rasterOn    = H->cfgBool("tpf2_bigmap", "street_raster", 1);
    g_octreeOn    = H->cfgBool("tpf2_bigmap", "octree",        1);
    g_octreeDepth = H->cfgInt ("tpf2_bigmap", "octree_depth", 11);
    g_placementAttempts = H->cfgInt("tpf2_bigmap", "placement_attempts", 200);
    g_worldEntryTimings = H->cfgBool("tpf2_bigmap", "world_entry_timings", 0) != 0;
    g_materialIndexFast = H->cfgBool("tpf2_bigmap", "material_index_fast", 0) != 0;
    g_terrainRefineFast = H->cfgBool("tpf2_bigmap", "terrain_refine_fast", 0) != 0;
    g_terrainMinMaxFast = H->cfgBool("tpf2_bigmap", "terrain_minmax_fast", 0) != 0;
    g_terrainAlignFast = H->cfgBool("tpf2_bigmap", "terrain_align_fast", 0) != 0;
    g_terrainCacheSpacing = H->cfgInt("tpf2_bigmap", "terrain_cache_spacing_m", 0);
    g_terrainCompress = H->cfgInt("tpf2_bigmap", "terrain_cache_compress", 0);
    g_terrainHotMB = H->cfgInt("tpf2_bigmap", "terrain_cache_hot_mb", 0);
    g_terrainWarmMB = H->cfgInt("tpf2_bigmap", "terrain_cache_warm_mb", -1);
    g_terrainMaxMB = H->cfgInt("tpf2_bigmap", "terrain_cache_max_mb", 0);
    g_simPhysicalMB = H->cfgInt("tpf2_bigmap", "simulate_physical_mb", 0);
    if (g_simPhysicalMB > 0) H->log("[tpf2_bigmap] SIMULATING a %d MiB machine for the pager policy (simulate_physical_mb; rig-only)", g_simPhysicalMB);
    g_terrainCowShare = H->cfgBool("tpf2_bigmap", "terrain_cow_share", 0) != 0;
    g_terrainDedupProbe = H->cfgBool("tpf2_bigmap", "terrain_dedup_probe", 0) != 0;
    g_terrainDedup = H->cfgBool("tpf2_bigmap", "terrain_dedup", 0) != 0;
    g_terrainLazyZero = H->cfgBool("tpf2_bigmap", "terrain_lazy_zero", 1) != 0;
    g_terrainBlocks = H->cfgBool("tpf2_bigmap", "terrain_blocks", 0) != 0;
    g_smallHotMB = H->cfgInt("tpf2_bigmap", "small_cache_hot_mb", 1024);
    if (g_smallHotMB < 64) g_smallHotMB = 64;
    g_terrainEvictPerSec = H->cfgInt("tpf2_bigmap", "terrain_cache_evict_per_s", 4000);
    g_materialEvictPerSec = H->cfgInt("tpf2_bigmap", "material_cache_evict_per_s", 4000);
    g_materialCompress = H->cfgInt("tpf2_bigmap", "material_cache_compress", 0);
    g_materialHotMB = H->cfgInt("tpf2_bigmap", "material_cache_hot_mb", 0);
    g_materialWarmMB = H->cfgInt("tpf2_bigmap", "material_cache_warm_mb", -1);
    g_materialMaxMB = H->cfgInt("tpf2_bigmap", "material_cache_max_mb", 0);
    // Auto budgets (hot 0, warm -1) are resolved at install; both imply a warm
    // allowance, so world-entry tracking must be on for them too.
    g_worldEntryTrackBusy = (g_terrainCompress==1 && (g_terrainWarmMB<0 || g_terrainWarmMB>g_terrainHotMB)) ||
                            (g_materialCompress==1 && (g_materialWarmMB<0 || g_materialWarmMB>g_materialHotMB));
    g_saveFast = H->cfgBool("tpf2_bigmap", "save_fast", 0) != 0;
    g_travelTimeLimit = H->cfgInt("tpf2_bigmap", "travel_time_limit_s", 0);
    g_alignmentBatch = H->cfgInt("tpf2_bigmap", "alignment_batch_tiles", 512);
    g_terrainServe = H->cfgBool("tpf2_bigmap", "terrain_sidecar", 1) != 0;
    g_sidecarWrite = H->cfgBool("tpf2_bigmap", "terrain_sidecar_write", 1) != 0;
    g_sidecarMaxTiles = H->cfgInt("tpf2_bigmap", "terrain_sidecar_max_tiles", 0);
    g_cargoPathTime = H->cfgInt("tpf2_bigmap", "cargo_path_time_s", 0);
    g_instanceShrink = H->cfgBool("tpf2_bigmap", "instance_shrink", 0) != 0;
    g_minimap = H->cfgBool("tpf2_bigmap", "minimap", 1) != 0;
    if (g_octreeDepth != 11 && g_octreeDepth != 12 && g_octreeDepth != 13) {
        H->log("octree_depth must be 11, 12 or 13; refusing invalid depth");
        return TPF2MP_ERR_FAILED;
    }
    {
        int budgetM = H->cfgInt("tpf2_bigmap", "cell_budget_millions", 1500);
        if (budgetM > 0) g_cellBudget = (double)budgetM * 1e6;
    }

    // Build checks come FIRST: both hooks need them, and the raster hook is
    // useful even when this plugin is not choosing the map size (the game's own
    // settings.lua worldDimensionsOverride reaches sizes that overflow too).
    // Read the ladder: one key per (size, format) cell we claim.
    for (int s = 0; s < 19; ++s) {   // 0..6 stock + 7..18 for added rows
        for (int f = 0; f < MAX_RATIOS; ++f) {
            char key[32];
            _snprintf_s(key, sizeof(key), _TRUNCATE, "size%d_format%d", s, f);
            const char* v = H->cfgStr("tpf2_bigmap", key, nullptr);
            if (!v || !*v) continue;
            int tx = 0, ty = 0;
            if (!ParseWxH(v, &tx, &ty)) {
                H->log("%s = '%s' is not <tiles>x<tiles> -- IGNORED", key, v);
                continue;
            }
            int sx = Sanitise(tx), sy = Sanitise(ty);
            BoundHeightmap(&sx, &sy);
            if (sx != tx || sy != ty) {
                H->log("%s: %dx%d adjusted to %dx%d (even, 2..%d; heightmap <= INT_MAX)",
                       key, tx, ty, sx, sy, EffectiveMaxTiles());
            }
            if (g_numClaims < MAX_CLAIMS) {
                g_claims[g_numClaims].size = s;
                g_claims[g_numClaims].format = f;
                g_claims[g_numClaims].tx = sx;
                g_claims[g_numClaims].ty = sy;
                ++g_numClaims;
            }
        }
    }

    // Added size rows: cfg add_size_rows=1, and each row is a claimed size >= 7
    // (contiguous from 7) with an optional size_label<N>.
    g_addSizeRows    = H->cfgBool("tpf2_bigmap", "add_size_rows", 1);
    g_newgameDensity = H->cfgBool("tpf2_bigmap", "newgame_density", 1);
    g_numExtraSizeRows = 0;
    if (g_addSizeRows) {
        for (int sz = 7; sz < 19 && g_numExtraSizeRows < 12; ++sz) {
            int fb = -1;
            for (int i = 0; i < g_numClaims; ++i)
                if (g_claims[i].size == sz && (fb < 0 || g_claims[i].format < g_claims[fb].format)) fb = i;
            if (fb < 0) break;                       // rows must be contiguous from 7
            char lk[24]; _snprintf_s(lk, sizeof lk, _TRUNCATE, "size_label%d", sz);
            const char* lbl = H->cfgStr("tpf2_bigmap", lk, nullptr);
            char def[16];
            if (!lbl || !*lbl) { _snprintf_s(def, sizeof def, _TRUNCATE, "%dx%d", g_claims[fb].tx, g_claims[fb].ty); lbl = def; }
            ExtraRow* r = &g_extraSizeRows[g_numExtraSizeRows++];
            r->size = sz;
            int j = 0; for (; lbl[j] && j < 15; ++j) r->label[j] = lbl[j]; r->label[j] = 0;
        }
        H->log("add_size_rows=1: %d extra size row(s) collected (need size7_format0.. claims)", g_numExtraSizeRows);
    }

    if (!H->moduleBase()) {
        H->log("not running inside TransportFever2.exe -- refusing to patch");
        return TPF2MP_ERR_BUILD;
    }
    g_base = H->moduleBase();
    // Build detection. host->buildOk() knows the Steam 35924 build. The GOG
    // build is a second measured binary; detect it by byte-verifying all three
    // sites at their GOG RVAs. A build that is neither fails here (and at the
    // per-site guards below) and is refused loudly -- the byte check, not the
    // PE timestamp, is the guard.
    g_gog = false;
    if (!H->buildOk()) {
        g_gog = H->verifyBytes(RVA_GETNUMTILES_GOG, EXPECTED, STEAL)
             && H->verifyBytes(RVA_RASTER_GOG, EXPECTED_RASTER, STEAL_RASTER)
             && H->verifyBytes(RVA_OCTREE_GOG, EXPECTED_OCTREE_GOG,
                               sizeof EXPECTED_OCTREE_GOG);
        if (g_gog) {
            H->log("game build is the GOG 2024-12-12 binary -- all three sites "
                   "byte-verify; using the GOG layout");
        } else {
            H->log("game build is neither Steam 35924 nor the GOG 2024-12-12 "
                   "binary -- every RVA here was measured on those two, so "
                   "refusing to patch (re-run the recon if the game updated)");
            SyncGameBaseMod(false);   // no density levels on an unknown build: stock file back
            SyncGameMinimapScript(false);   // nor a minimap button whose hooks cannot install
            return TPF2MP_ERR_BUILD;
        }
    }

    int installed = 0;

    // ---- New Game page: density levels (independent of the size config) ----
    installed += InstallDensityLevels();

    // RandomLocationFactory serves preview town/industry placement. This is
    // independent of the runtime octree, and overflows at ~185 km separation.
    installed += InstallPlacementSpacing();
    installed += InstallFastPlacement();
    installed += InstallWorldEntryTimings();
    installed += InstallMaterialIndexFast();
    installed += InstallTerrainRefineFast();
    installed += InstallTerrainMinMaxFast();
    installed += InstallTerrainAlignFast();
    installed += InstallTerrainCache();
    installed += InstallTerrainCompression();
    installed += InstallTerrainBlocks();
    installed += InstallMaterialCompression();
    installed += InstallSaveFast();
    installed += InstallTravelTime();
    installed += InstallAlignmentBatch();
    installed += InstallTerrainServe();
    installed += InstallSidecarIo();
    installed += InstallInstanceShrink();
    installed += InstallMinimap();

    // ---- street occupancy raster: scale cell size with map size -----------
    if (g_rasterOn) {
        uintptr_t rva = g_gog ? RVA_RASTER_GOG : RVA_RASTER;
        if (!H->verifyBytes(rva, EXPECTED_RASTER, STEAL_RASTER)) {
            H->log("raster: prologue mismatch at RVA 0x%llx -- NOT hooked; maps "
                   "over ~46.1 km will abort in Creating streets",
                   (unsigned long long)rva);
        } else {
            void* t = nullptr;
            if (H->installHook(H->moduleBase() + rva, (void*)&RasterDetour,
                               STEAL_RASTER, &t)) {
                g_origRaster = (RasterCtorFn)t;
                installed++;
                H->log("raster: hooked street occupancy ctor at %p, budget %.2fe9 "
                       "cells (1 m grid kept below that; coarsened above)",
                       (void*)(H->moduleBase() + rva), g_cellBudget / 1e9);
            } else {
                H->log("raster: installHook failed -- NOT hooked");
            }
        }
    } else {
        H->log("raster: disabled (street_raster=0); the int32 overflow at ~46.1 km "
               "is live -- keep makeInitialStreets=false above that size");
    }

    // ---- octree root box: +-32768 m -> +-65536 m for maps over 256 tiles ---
    {
        int biggest = (g_tilesX > g_tilesY) ? g_tilesX : g_tilesY;
        for (int i = 0; i < g_numClaims; ++i) {
            if (g_claims[i].tx > biggest) biggest = g_claims[i].tx;
            if (g_claims[i].ty > biggest) biggest = g_claims[i].ty;
        }
        // An added row's narrow ratios run longer than its square (128 at 1:5 is
        // 58 x 290), so they count too.
        for (int r = 0; r < g_numExtraSizeRows; ++r) {
            for (int f = 0; f < g_maxRatio; ++f) {
                int tx = 0, ty = 0;
                RowShape(g_extraSizeRows[r].size, f, &tx, &ty);
                if (tx > biggest) biggest = tx;
                if (ty > biggest) biggest = ty;
            }
        }
        // Extended formats also apply to stock rows and explicit square
        // claims, even when no added size rows are configured.
        if(g_maxRatio>5)for(int f=5;f<g_maxRatio;++f) {
            int tx,ty;DeriveShape(96,f,EffectiveMaxTiles(),&tx,&ty);
            if(ty>biggest)biggest=ty;
            for(int size=0;size<19;++size)if(FindClaim(size,0)>=0) {
                RowShape(size,f,&tx,&ty);
                if(tx>biggest)biggest=tx;if(ty>biggest)biggest=ty;
            }
        }
        uintptr_t rva = g_gog ? RVA_OCTREE_GOG : RVA_OCTREE;
        const uint8_t* exp = g_gog ? EXPECTED_OCTREE_GOG : EXPECTED_OCTREE;
        if (!g_octreeOn) {
            H->log("octree: disabled (octree=0); maps over %d tiles will grow "
                   "duplicate street nodes past +-32,768 m", OCTREE_STOCK_TILES);
        } else if (g_octreeDepth >= 12) {
            // Explicit opt-in also applies on load, even with a small menu
            // ladder: saved worlds do not pass through GetNumTilesNew.
            if (!InstallOctDepth12(rva, exp)) {
                H->log("octree depth %d: installation failed; large-map menu refused", g_octreeDepth);
                return TPF2MP_ERR_FAILED;
            }
            installed++;
        } else if (biggest <= OCTREE_STOCK_TILES) {
            H->log("octree: not needed (largest configured size %d <= %d tiles), "
                   "shipped +-32,768 m root left alone", biggest, OCTREE_STOCK_TILES);
        } else if (!H->verifyBytes(rva, exp, 13)) {
            H->log("octree: byte mismatch at RVA 0x%llx -- NOT patched; sizes over "
                   "%d tiles WILL corrupt street nodes near the edge",
                   (unsigned long long)rva, OCTREE_STOCK_TILES);
        } else if (!H->patchBytes(rva, PATCH_OCTREE, sizeof PATCH_OCTREE)) {
            H->log("octree: patchBytes failed at RVA 0x%llx -- NOT patched",
                   (unsigned long long)rva);
        } else {
            installed++;
            H->log("octree: root box widened +-32,768 -> +-65,536 m (depth 10 -> 11) "
                   "at RVA 0x%llx; ceiling is now %d tiles",
                   (unsigned long long)rva, OCTREE_PATCH_TILES);
            if (biggest > OCTREE_PATCH_TILES)
                H->log("octree: WARNING configured size %d exceeds %d tiles -- the "
                       "patched box does not reach that far either", biggest,
                       OCTREE_PATCH_TILES);
        }
    }

    // ---- map size ----------------------------------------------------------
    if (g_numClaims > 0) {
        H->log("map-size ladder: %d cell(s) claimed", g_numClaims);
        for (int i = 0; i < g_numClaims; ++i) {
            H->log("   size=%d format=%d -> %d x %d tiles = %.1f x %.1f km "
                   "(heightmap %d x %d px)",
                   g_claims[i].size, g_claims[i].format,
                   g_claims[i].tx, g_claims[i].ty,
                   g_claims[i].tx * 0.256, g_claims[i].ty * 0.256,
                   g_claims[i].tx * 64 + 1, g_claims[i].ty * 64 + 1);
        }
    }
    if (g_numClaims == 0 && (g_tilesX <= 0 || g_tilesY <= 0) && g_maxRatio<=5) {
        H->log("no map size configured -- set size<S>_format<F> = <w>x<h> "
               "(or tiles_x/tiles_y) in [tpf2_bigmap] of tpf2mp.cfg");
        return installed ? TPF2MP_OK : TPF2MP_ERR_DISABLED;
    }

    int wantX = Sanitise(g_tilesX > 0 ? g_tilesX : 2);
    int wantY = Sanitise(g_tilesY > 0 ? g_tilesY : 2);
    BoundHeightmap(&wantX, &wantY);
    if (g_tilesX > 0 && g_tilesY > 0 && (wantX != g_tilesX || wantY != g_tilesY)) {
        H->log("requested %d x %d adjusted to %d x %d (even, 2..%d; heightmap <= INT_MAX)",
               g_tilesX, g_tilesY, wantX, wantY, EffectiveMaxTiles());
    }
    if(g_tilesX>0 && g_tilesY>0){g_tilesX = wantX; g_tilesY = wantY;}

    // The stock 1 m street raster is a vector<bool> sized by a 32-bit multiply
    // over the map's metre bbox, so it overflows when (tx*256+1)*(ty*256+1) >
    // INT_MAX. That is a PER-MAP PRODUCT, not a per-axis cap: a long thin
    // rectangle is legal where its long edge squared is not. Only a risk when
    // the raster hook is off (street_raster=0); the hook grows the cell so the
    // product never reaches the game's multiply.
    if (!g_origRaster) {
        int mx[MAX_CLAIMS + 1], my[MAX_CLAIMS + 1], nm = 0;
        if (g_tilesX > 0 && g_tilesY > 0) { mx[nm] = g_tilesX; my[nm] = g_tilesY; ++nm; }
        for (int i = 0; i < g_numClaims; ++i) { mx[nm] = g_claims[i].tx; my[nm] = g_claims[i].ty; ++nm; }
        for (int i = 0; i < nm; ++i) {
            long long cells = (long long)(mx[i] * 256 + 1) * (long long)(my[i] * 256 + 1);
            if (cells > 2147483647LL) {
                H->log("WARNING: %d x %d tiles (%.1f x %.1f km) overflows the stock 1 m "
                       "street raster (%.2fe9 cells > INT_MAX) and the raster hook is NOT "
                       "active. Generation will abort unless street_raster=1 (recommended) "
                       "or makeInitialStreets=false in res/config/base_config.lua.",
                       mx[i], my[i], mx[i] * 0.256, my[i] * 0.256, cells / 1e9);
            }
        }
    }

    uintptr_t rva = g_gog ? RVA_GETNUMTILES_GOG : RVA_GETNUMTILES;
    if (!H->verifyBytes(rva, EXPECTED, STEAL)) {
        H->log("prologue mismatch at RVA 0x%llx -- refusing to patch",
               (unsigned long long)rva);
        return installed ? TPF2MP_OK : TPF2MP_ERR_BUILD;
    }

    void* tramp = nullptr;
    uintptr_t target = H->moduleBase() + rva;
    if (!H->installHook(target, (void*)&Detour, STEAL, &tramp)) {
        H->log("installHook failed at %p", (void*)target);
        return installed ? TPF2MP_OK : TPF2MP_ERR_FAILED;
    }
    g_orig = (GetNumTilesFn)tramp;
    installed++;

    // ---- New Game page: size rows (need the detour above) -----------------
    installed += InstallSizeRows();
    installed += InstallMapRatios();

    H->log("hooked GetNumTilesNew at %p (tramp %p)", (void*)target, tramp);
    H->log("size index %d, ratio index %d -> %d x %d tiles = %.1f x %.1f km "
           "(heightmap %d x %d px)",
           g_sizeIndex, g_formatIndex, g_tilesX, g_tilesY,
           g_tilesX * 0.256, g_tilesY * 0.256,
           g_tilesX * 64 + 1, g_tilesY * 64 + 1);
    return TPF2MP_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
