// Steam 35924 only. Included after H and the stock octree patch definitions.
// See docs/octree-depth12.md for the binary audit and remaining live checks.
static int g_octreeDepth = 11;
static bool g_depth12Active = false;

// What this build can actually install. Depths 12 and 13 replace the root
// descent prologue and the level builder, and both replacements are Steam-35924
// code shapes: the GOG build has neither at a verified address, so there the
// shipped depth-11 widening is the ceiling. Everything that sizes the menu has
// to agree with what really went in -- see the octree block in Tpf2mpPluginInit.
static int EffectiveOctreeDepth()
{
    return (g_gog && g_octreeDepth >= 12) ? 11 : g_octreeDepth;
}

static volatile LONG g_compactNext[2] = { 0x4fffffff, 0x5fffffff };
static const uintptr_t RVA_OCT_DESCEND = 0xa507e0;
static const uintptr_t RVA_OCT_LEVEL = 0x853d30;
static const uint8_t EXPECTED_OCT_DESCEND[21] = {
    0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,
    0x56,0x41,0x57,0x48,0x81,0xec,0xf0,0x00,0x00,0x00
};
static const uint8_t EXPECTED_OCT_LEVEL[39] = {
    0x41,0xb8,0x01,0x00,0x00,0x00,0x45,0x8b,0xcb,0x45,0x8b,0xe3,
    0x41,0x3b,0xd0,0x7c,0x16,0x45,0x03,0xc8,0x41,0xff,0xc4,
    0x46,0x8d,0x04,0xc5,0x00,0x00,0x00,0x00,0x43,0x8d,0x04,0x08,
    0x3b,0xd0,0x7d,0xea
};

// Depth counts the root. A level-10 parent has ID >= 0x09249249.
// Stock IDs end at 0x49249248. Level 11 uses 0x50000000..0x5fffffff;
// level 12 uses 0x60000000..0x6fffffff. Level 11 may now be INTERNAL.
// Decode the parent stored in memory, never the overflowed incoming child ID.
// Never recycle IDs: queued renderer work may retain them.
static int Depth12Id(int incoming, int parentId, bool hasParent,
                     int remaining, int existingId, bool exists)
{
    if (!hasParent || parentId < 0x09249249)
        return incoming;
    int bank = parentId <= 0x49249248 ? 0 :
        (parentId >= 0x50000000 && parentId <= 0x5fffffff ? 1 : -1);
    if (bank < 0 || remaining < 1) __fastfail(7);
    if (exists) return existingId;
    LONG id = InterlockedIncrement(&g_compactNext[bank]);
    if ((uint32_t)id >= (bank == 0 ? 0x60000000u : 0x70000000u)) {
        if (H) H->log("octree: compact ID space exhausted at level %d", bank + 11);
        __fastfail(7); // Never publish a negative or recycled identity.
    }
    return (int)id;
}

// RCX is forwarded verbatim (the original recursive helper preserves it).
// RDX is the explicit 16-byte iterator-result buffer; five further arguments
// are at entry RSP+0x28..0x48. Verified against both call sites and prologue.
typedef void* (__fastcall *OctDescendFn)(void*, void*, void*, int,
    const float*, const float*, int, float*, void**);
static OctDescendFn g_origOctDescend = nullptr;
static void* __fastcall OctDescendDetour(void* context, void* result,
    void* parent, int index, const float* center, const float* extent,
    int remaining, float* box, void** slot)
{
    if (g_depth12Active) {
        int parentId = parent ? *(int*)((uint8_t*)parent + 8) : 0;
        int existingId = *slot ? *(int*)((uint8_t*)*slot + 8) : 0;
        index = Depth12Id(index, parentId, parent != nullptr,
                          remaining, existingId, *slot != nullptr);
    }
    return g_origOctDescend(context, result, parent, index, center, extent,
                            remaining, box, slot);
}

static void OctAbsoluteJump(uint8_t* dst, uintptr_t target)
{
    dst[0] = 0xff; dst[1] = 0x25;
    memset(dst + 2, 0, 4);
    memcpy(dst + 6, &target, 8);
}

// No stack changes and no call: retain the game's unwind state and register
// contract. The two relative branches in the copied stock loop remain valid;
// its exit lands on our final absolute jump back to RVA 0x853d57.
static size_t BuildOctLevelStub(uint8_t* dst, uintptr_t back)
{
    const uint8_t prefix[] = {
        0x81,0xfa,0x00,0x00,0x00,0x50, // cmp edx,0x50000000
        0x72,0x19,                     // jb stock loop, at +33
        0x41,0x89,0xd4,                // mov r12d,edx
        0x41,0xc1,0xec,0x1c,           // shr r12d,28
        0x41,0x83,0xc4,0x06            // add r12d,6 (5 -> 11; 6 -> 12)
    };
    memcpy(dst, prefix, sizeof prefix);
    OctAbsoluteJump(dst + 19, back);
    memcpy(dst + 33, EXPECTED_OCT_LEVEL, sizeof EXPECTED_OCT_LEVEL);
    OctAbsoluteJump(dst + 72, back);
    return 86;
}

static bool InstallOctDepth12(uintptr_t rootRva, const uint8_t* rootExpected)
{
    // Preflight every site before touching anything. The root/depth change is
    // published LAST; a partial installation remains a stock-ID pass-through.
    if ((g_octreeDepth != 12 && g_octreeDepth != 13) || g_gog
        || !H->verifyBytes(rootRva, rootExpected, 13)
        || !H->verifyBytes(RVA_OCT_DESCEND, EXPECTED_OCT_DESCEND, 21)
        || !H->verifyBytes(RVA_OCT_LEVEL, EXPECTED_OCT_LEVEL, 39)) {
        H->log("octree: unsupported compact depth/build or byte mismatch; refused");
        return false;
    }
    uint8_t* stub = (uint8_t*)VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stub) return false;
    size_t n = BuildOctLevelStub(stub, H->moduleBase() + RVA_OCT_LEVEL + 39);
    DWORD old = 0;
    if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, n);
    void* trampoline = nullptr;
    if (!H->installHook(H->moduleBase() + RVA_OCT_DESCEND,
                        (void*)&OctDescendDetour, 21, &trampoline)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    g_origOctDescend = (OctDescendFn)trampoline;
    uint8_t jump[39]; memset(jump, 0x90, sizeof jump);
    OctAbsoluteJump(jump, (uintptr_t)stub);
    if (!H->patchBytes(RVA_OCT_LEVEL, jump, sizeof jump)) return false;
    uint8_t root[13]; memcpy(root, PATCH_OCTREE, sizeof root);
    const float halfExtent = g_octreeDepth == 13 ? 262144.0f : 131072.0f;
    memcpy(root + 1, &halfExtent, 4);
    root[12] = (uint8_t)g_octreeDepth;
    if (!H->patchBytes(rootRva, root, sizeof root)) return false;
    g_depth12Active = true;
    H->log("octree: EXPERIMENTAL depth %d, root +-%.0f m, 128 m leaves; "
           "%d-tile edge capacity; compact IDs enabled", g_octreeDepth,
           halfExtent, g_octreeDepth == 13 ? 2048 : 1024);
    return true;
}

extern "C" __declspec(dllexport)
int BigmapTestOctreeId(int incoming, int parentId, int hasParent,
                      int remaining, int existingId, int exists)
{
    return Depth12Id(incoming, parentId, hasParent != 0, remaining,
                     existingId, exists != 0);
}
extern "C" __declspec(dllexport)
size_t BigmapTestOctreeLevelStub(uint8_t* dst, uintptr_t back)
{
    return BuildOctLevelStub(dst, back);
}

extern "C" __declspec(dllexport)
void* BigmapTestOctreeDescend(OctDescendFn original, void* context, void* result,
    void* parent, int index, const float* center, const float* extent,
    int remaining, float* box, void** slot)
{
    OctDescendFn saved = g_origOctDescend;
    bool active = g_depth12Active;
    g_origOctDescend = original; g_depth12Active = true;
    void* out = OctDescendDetour(context, result, parent, index, center, extent,
                                remaining, box, slot);
    g_origOctDescend = saved; g_depth12Active = active;
    return out;
}

extern "C" __declspec(dllexport)
int BigmapTestInstallOctree12(const Tpf2mpHost* host, int gog, int depth)
{
    const Tpf2mpHost* savedHost = H;
    OctDescendFn savedOriginal = g_origOctDescend;
    bool savedActive = g_depth12Active, savedGog = g_gog;
    int savedDepth = g_octreeDepth;
    H = host; g_gog = gog != 0; g_octreeDepth = depth;
    bool ok = InstallOctDepth12(RVA_OCTREE, EXPECTED_OCTREE);
    H = savedHost; g_gog = savedGog; g_octreeDepth = savedDepth;
    g_origOctDescend = savedOriginal; g_depth12Active = savedActive;
    return ok ? 1 : 0;
}
