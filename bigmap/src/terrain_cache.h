// Steam 35924: choose the derived height-cache resolution before terrain binds.
// The ECS Terrain field is authoritative; changing only CTerrain's cached copy
// would leave allocation, refinement and terrain-alignment code inconsistent.
#pragma once
#include <vector>

struct TerrainUploadVector { const uint16_t *first, *last, *end; };
struct TerrainUploadData { const TerrainUploadVector* data; int32_t x, y; };
using TerrainUploadFn = void (__fastcall*)(void*, const TerrainUploadData*, const void*);
static TerrainUploadFn g_originalTerrainUpload = nullptr;

// CPU border samples cover -2..258 m; the stock GPU buffer covers -1..257 m.
// Thus destination index d samples source coordinate (d+1)/2, including borders.
static void ExpandTerrainUpload(const uint16_t* src, uint16_t* dst) {
    for (unsigned y=0;y<259;++y) {
        const unsigned sy=(y+1)/2, fy=(y+1)&1;
        for (unsigned x=0;x<259;++x) {
            const unsigned sx=(x+1)/2, fx=(x+1)&1;
            const unsigned a=src[sy*131+sx], b=src[sy*131+sx+1];
            const unsigned c=src[(sy+1)*131+sx], d=src[(sy+1)*131+sx+1];
            dst[y*259+x]=uint16_t(((2-fy)*((2-fx)*a+fx*b)+fy*((2-fx)*c+fx*d)+2)/4);
        }
    }
}
static void __fastcall TerrainUploadDetour(void* self,const TerrainUploadData* input,const void* texture) {
    const auto p=static_cast<const uint8_t*>(self);
    // Exact observed mismatch only. Other grids and already matching uploads
    // preserve the original path, arguments, errors and ownership.
    if (self && input && input->data && input->data->first && !input->x && !input->y &&
        reinterpret_cast<uintptr_t>(input->data->last)-reinterpret_cast<uintptr_t>(input->data->first)==131*131*2 &&
        *reinterpret_cast<const int32_t*>(p+0x14)==257 &&
        *reinterpret_cast<const int32_t*>(p+0x18)==257 &&
        *reinterpret_cast<const int32_t*>(p+0x1c)==1 &&
        *reinterpret_cast<const int32_t*>(p+0x20)==1) {
        std::vector<uint16_t> expanded(259*259);
        ExpandTerrainUpload(input->data->first,expanded.data());
        TerrainUploadVector view{expanded.data(),expanded.data()+expanded.size(),expanded.data()+expanded.size()};
        TerrainUploadData data{&view,0,0};
        // Original Vulkan upload copies to staging synchronously; the stock
        // caller also reuses its input buffer immediately after this returns.
        g_originalTerrainUpload(self,&data,texture);
        return;
    }
    g_originalTerrainUpload(self,input,texture);
}
static const uint8_t kTerrainUploadBytes[]={0x48,0x83,0xec,0x38,0x83,0x7a,0x08,0x00,0x4c,0x8b,0xca,0x4c,0x8b,0xd9};

static int g_terrainCacheSpacing = 0; // 0 = saved/default, 1 = 1 m, 2 = 2 m
struct BigmapTerrainComponent {
    int32_t tilesX, tilesY, baseLevels;
    float baseStepX, baseStepY, heightScale;
    int32_t highLevels;
    float offsetZ;
    int32_t riverSetting;
};
static_assert(sizeof(BigmapTerrainComponent) == 0x24, "Steam Terrain layout");
// The alignment worker (aac460 -> 33d090 -> 3c4620) builds metre-indexed
// blocks, independent of highLevels. Refine those at 1 m, then decimate their
// final aligned heights at publication. Never change the shared ECS component.
using TerrainRefineFn=void(__fastcall*)(void*,const BigmapTerrainComponent*,uint64_t,int,int,int,int,uint16_t*,int,int,int);
static TerrainRefineFn g_originalTerrainRefine=nullptr;
static bool IsCoarseTerrain(const BigmapTerrainComponent* t) {
    return t && t->baseLevels==6 && t->highLevels==7 && t->baseStepX==4.f && t->baseStepY==4.f;
}
static void __fastcall TerrainRefineDetour(void* terrain,const BigmapTerrainComponent* t,uint64_t tile,
    int x0,int y0,int x1,int y1,uint16_t* out,int stride,int dx,int dy) {
    if(IsCoarseTerrain(t)) {
        auto fine=*t;fine.highLevels=8;
        g_originalTerrainRefine(terrain,&fine,tile,x0,y0,x1,y1,out,stride,dx,dy);
    } else g_originalTerrainRefine(terrain,t,tile,x0,y0,x1,y1,out,stride,dx,dy);
}
struct TerrainHeightRegion {const uint16_t* data;int32_t width,height,x,y;};
struct TerrainHeightRegions {const TerrainHeightRegion *first,*last,*end;};
using TerrainCachePublishFn=void(__fastcall*)(void*,const TerrainHeightRegions*);
static TerrainCachePublishFn g_originalTerrainCachePublish=nullptr;
static int HalfCeil(int v) {return v/2+(v>0 && v%2);}
static int HalfFloor(int v) {return v/2-(v<0 && v%2);}
static void __fastcall TerrainCachePublishDetour(void* self,const TerrainHeightRegions* input) {
    if(!self || !input || !IsCoarseTerrain(reinterpret_cast<const BigmapTerrainComponent*>(static_cast<const uint8_t*>(self)+0x20))) {
        g_originalTerrainCachePublish(self,input);return;
    }
    std::vector<std::vector<uint16_t>> storage;
    std::vector<TerrainHeightRegion> regions;
    for(auto r=input->first;r!=input->last;++r) {
        const int x=HalfCeil(r->x),y=HalfCeil(r->y);
        const int w=HalfFloor(r->x+r->width-1)-x+1,h=HalfFloor(r->y+r->height-1)-y+1;
        if(w<=0 || h<=0)continue;
        storage.emplace_back(size_t(w)*h);auto& dst=storage.back();
        for(int j=0;j<h;++j)for(int i=0;i<w;++i)
            dst[size_t(j)*w+i]=r->data[size_t(2*(y+j)-r->y)*r->width+2*(x+i)-r->x];
        regions.push_back({dst.data(),w,h,x,y});
    }
    const TerrainHeightRegions converted{regions.data(),regions.data()+regions.size(),regions.data()+regions.size()};
    g_originalTerrainCachePublish(self,&converted);
}
static const uint8_t kTerrainRefineBytes[]={0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0x98};
static const uint8_t kTerrainPublishBytes[]={0x48,0x8b,0xc4,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0x88};
// Construction alignment comparison also uses metre-indexed rectangles.
// Its two direct reads must agree on a temporary 257x257 view (2146540).
using TerrainLevelFn=int(__fastcall*)(void*);
using TerrainVerticesFn=const TerrainUploadVector*(__fastcall*)(void*,uint64_t);
static TerrainLevelFn g_originalTerrainLevel=nullptr;
static TerrainVerticesFn g_originalTerrainVertices=nullptr;
static int __fastcall TerrainLevelDetour(void* self) {
    const int level=g_originalTerrainLevel(self);
    return level==7 && uintptr_t(_ReturnAddress())==H->moduleBase()+0x214674f ? 8 : level;
}
static const TerrainUploadVector* TerrainComparisonVertices(const TerrainUploadVector* source) {
    if(!source || size_t(source->last-source->first)!=129*129)return source;
    static thread_local std::vector<uint16_t> fine(257*257);
    static thread_local TerrainUploadVector view;
    for(unsigned y=0;y<257;++y)for(unsigned x=0;x<257;++x) {
        const unsigned sx=x/2,sy=y/2,fx=x&1,fy=y&1;
        const unsigned nx=sx+(sx<128),ny=sy+(sy<128);
        const auto* s=source->first;
        fine[y*257+x]=uint16_t(((2-fy)*((2-fx)*s[sy*129+sx]+fx*s[sy*129+nx])+fy*((2-fx)*s[ny*129+sx]+fx*s[ny*129+nx])+2)/4);
    }
    view={fine.data(),fine.data()+fine.size(),fine.data()+fine.size()};return &view;
}
static const TerrainUploadVector* __fastcall TerrainVerticesDetour(void* self,uint64_t tile) {
    const bool comparison=uintptr_t(_ReturnAddress())==H->moduleBase()+0x214676d;
    const auto* source=g_originalTerrainVertices(self,tile);
    return comparison ? TerrainComparisonVertices(source) : source;
}
static const uint8_t kTerrainLevelBytes[]={0x8b,0x41,0x38,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
static const uint8_t kTerrainVerticesBytes[]={0x4c,0x8b,0x41,0x18,0x48,0x8b,0xc2,0x48,0xc1,0xe8,0x20,0x41,0x2b,0x40,0x04};
using TerrainBindFn = void (__fastcall*)(void*, int32_t);
using TerrainComponentFn = BigmapTerrainComponent* (__fastcall*)(void*, const int32_t*);
static TerrainBindFn g_originalTerrainBind = nullptr;
static TerrainComponentFn g_getTerrainComponent = nullptr;

static bool TerrainCacheIsEmpty(const uint8_t* bytes) {
    const auto grid = *reinterpret_cast<const uint8_t* const*>(bytes + 0x18);
    // CTerrain's constructor allocates a zero-dimension Grid, not a null one.
    return !grid || (*reinterpret_cast<const uint64_t*>(grid) == 0 &&
        *reinterpret_cast<const uint64_t*>(grid + 8) == 0 &&
        *reinterpret_cast<void* const*>(grid + 0x10) == *reinterpret_cast<void* const*>(grid + 0x18));
}

static bool SelectTerrainCacheLevel(BigmapTerrainComponent* t, int spacing) {
    if (!t || (spacing != 1 && spacing != 2)) return false;
    // Only the measured stock 256 m tile / 4 m source layout. In particular,
    // never allow highLevels <= baseLevels (the game's refinement asserts).
    if (t->tilesX <= 0 || t->tilesY <= 0 || t->tilesX > 2048 || t->tilesY > 2048 ||
        t->baseLevels != 6 || t->baseStepX != 4.0f || t->baseStepY != 4.0f ||
        (t->highLevels != 7 && t->highLevels != 8)) return false;
    const int requested = spacing == 2 ? 7 : 8;
    if (t->highLevels == requested) return false;
    t->highLevels = requested;
    return true;
}

static void __fastcall TerrainBindDetour(void* self, int32_t entity) {
    const auto bytes = static_cast<uint8_t*>(self);
    // SetTerrainEntity is a one-time binding on a fresh CTerrain, both when
    // creating a world and when loading one. Never resize an existing cache.
    if (self && entity >= 0 && *reinterpret_cast<int32_t*>(bytes + 0x10) == -1 &&
        TerrainCacheIsEmpty(bytes)) {
        void* engine = *reinterpret_cast<void**>(bytes + 8);
        if (engine) {
            auto* t = g_getTerrainComponent(engine, &entity);
            if (SelectTerrainCacheLevel(t, g_terrainCacheSpacing)) {
                const uint64_t tiles = uint64_t(t->tilesX) * t->tilesY;
                const double before = double(tiles * 257 * 257 * 2) / 1073741824.0;
                const double after = double(tiles * 129 * 129 * 2) / 1073741824.0;
                H->log("terrain cache: %d m, highLevels=%d, %dx%d tiles; "
                       "1 m/2 m payload %.3f/%.3f GiB (shared between views); "
                       "base heightmap unchanged", g_terrainCacheSpacing, t->highLevels,
                       t->tilesX, t->tilesY, before, after);
            }
        }
    }
    g_originalTerrainBind(self, entity);
}

static const uint8_t kTerrainBindBytes[] = {
    0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff
};
static const uint8_t kTerrainComponentBytes[] = {
    0x4c,0x8b,0xdc,0x57,0x48,0x81,0xec,0xa0,0,0,0,0x49,0xc7,0x43,0x88,0xfe,0xff,0xff,0xff
};
static bool InstallTerrainCache() {
    if (g_gog) return false;
    if (g_terrainCacheSpacing < 0 || g_terrainCacheSpacing > 2) {
        H->log("terrain cache: invalid terrain_cache_spacing_m (use 0, 1 or 2); OFF");
        return false;
    }
    // Normal 1 m mode needs only restoration before binding. Do not install
    // the abandoned 2 m renderer/alignment/construction experiment for it.
    if(g_terrainCacheSpacing==1) {
        if(!H->verifyBytes(0x33da70,kTerrainBindBytes,sizeof kTerrainBindBytes) ||
           !H->verifyBytes(0x112210,kTerrainComponentBytes,sizeof kTerrainComponentBytes))return false;
        g_getTerrainComponent=reinterpret_cast<TerrainComponentFn>(H->moduleBase()+0x112210);
        if(!H->installHook(H->moduleBase()+0x33da70,reinterpret_cast<void*>(TerrainBindDetour),
                          sizeof kTerrainBindBytes,reinterpret_cast<void**>(&g_originalTerrainBind)))return false;
        H->log("terrain cache: stock 1 m resolution restored on new/load binding");return true;
    }
    if (!H->verifyBytes(0x347090,kTerrainUploadBytes,sizeof kTerrainUploadBytes) ||
        !H->verifyBytes(0x3c4620,kTerrainRefineBytes,sizeof kTerrainRefineBytes) ||
        !H->verifyBytes(0x33cd10,kTerrainPublishBytes,sizeof kTerrainPublishBytes) ||
        !H->verifyBytes(0x33d330,kTerrainLevelBytes,sizeof kTerrainLevelBytes) ||
        !H->verifyBytes(0x33d7c0,kTerrainVerticesBytes,sizeof kTerrainVerticesBytes) ||
        !H->verifyBytes(0x33da70, kTerrainBindBytes, sizeof kTerrainBindBytes) ||
        !H->verifyBytes(0x112210, kTerrainComponentBytes, sizeof kTerrainComponentBytes)) {
        H->log("terrain cache: Steam 35924 byte mismatch; OFF"); return false;
    }
    // Install the renderer bridge first: never coarsen CPU caches without it.
    if (!H->installHook(H->moduleBase()+0x347090,(void*)&TerrainUploadDetour,
                       sizeof kTerrainUploadBytes,(void**)&g_originalTerrainUpload)) {
        H->log("terrain cache: upload bridge failed; cache override OFF");return false;
    }
    H->log("terrain cache: 2 m CPU to 1 m GPU upload bridge enabled");
    // Publish first: a failed refinement installation cannot introduce metre
    // blocks into a coarse cache. No bindings/world loading have started yet.
    if(!H->installHook(H->moduleBase()+0x33cd10,(void*)&TerrainCachePublishDetour,sizeof kTerrainPublishBytes,(void**)&g_originalTerrainCachePublish) ||
       !H->installHook(H->moduleBase()+0x3c4620,(void*)&TerrainRefineDetour,sizeof kTerrainRefineBytes,(void**)&g_originalTerrainRefine)) {
        H->log("terrain cache: alignment bridge failed; cache override OFF");return false;
    }
    H->log("terrain cache: metre alignment to 2 m cache coordinate bridge enabled");
    if(!H->installHook(H->moduleBase()+0x33d7c0,(void*)&TerrainVerticesDetour,sizeof kTerrainVerticesBytes,(void**)&g_originalTerrainVertices) ||
       !H->installHook(H->moduleBase()+0x33d330,(void*)&TerrainLevelDetour,sizeof kTerrainLevelBytes,(void**)&g_originalTerrainLevel)) {
        H->log("terrain cache: construction comparison bridge failed; cache override OFF");return false;
    }
    if (!g_terrainCacheSpacing) return true; // also supports saved level-7 worlds
    g_getTerrainComponent = reinterpret_cast<TerrainComponentFn>(H->moduleBase() + 0x112210);
    if (!H->installHook(H->moduleBase() + 0x33da70, (void*)&TerrainBindDetour,
                       sizeof kTerrainBindBytes, (void**)&g_originalTerrainBind)) {
        H->log("terrain cache: binding hook failed; OFF"); return false;
    }
    H->log("terrain cache: %d m requested for new/load bindings; experimental", g_terrainCacheSpacing);
    return true;
}

extern "C" __declspec(dllexport)
const TerrainUploadVector* BigmapTestTerrainComparison(const TerrainUploadVector* source) {return TerrainComparisonVertices(source);}
extern "C" __declspec(dllexport)
void BigmapTestTerrainPublish(void* self,const TerrainHeightRegions* input,TerrainCachePublishFn fn) {
    auto old=g_originalTerrainCachePublish;g_originalTerrainCachePublish=fn;TerrainCachePublishDetour(self,input);g_originalTerrainCachePublish=old;
}
extern "C" __declspec(dllexport)
void BigmapTestTerrainRefine(void* terrain,const BigmapTerrainComponent* t,uint64_t tile,int x0,int y0,int x1,int y1,uint16_t* out,int stride,int dx,int dy,TerrainRefineFn fn) {
    auto old=g_originalTerrainRefine;g_originalTerrainRefine=fn;TerrainRefineDetour(terrain,t,tile,x0,y0,x1,y1,out,stride,dx,dy);g_originalTerrainRefine=old;
}
extern "C" __declspec(dllexport)
void BigmapTestExpandTerrainUpload(const uint16_t* source,uint16_t* destination) {
    ExpandTerrainUpload(source,destination);
}
extern "C" __declspec(dllexport)
void BigmapTestTerrainUpload(void* self,const TerrainUploadData* data,const void* texture,TerrainUploadFn fn) {
    auto old=g_originalTerrainUpload;g_originalTerrainUpload=fn;
    TerrainUploadDetour(self,data,texture);g_originalTerrainUpload=old;
}

extern "C" __declspec(dllexport)
int BigmapTestSelectTerrainCache(BigmapTerrainComponent* t, int spacing) {
    return SelectTerrainCacheLevel(t, spacing);
}
extern "C" __declspec(dllexport)
int BigmapTestInstallTerrainCache(const Tpf2mpHost* host, int gog, int spacing) {
    const auto oldHost=H; const bool oldGog=g_gog; const int oldSpacing=g_terrainCacheSpacing;
    H=host; g_gog=gog!=0; g_terrainCacheSpacing=spacing;
    bool ok=InstallTerrainCache();
    H=oldHost; g_gog=oldGog; g_terrainCacheSpacing=oldSpacing;
    return ok;
}
extern "C" __declspec(dllexport)
void BigmapTestBindTerrain(const Tpf2mpHost* host, int spacing, void* self, int32_t entity,
                          TerrainComponentFn getter, TerrainBindFn original) {
    const auto oldHost=H; const int oldSpacing=g_terrainCacheSpacing;
    const auto oldGetter=g_getTerrainComponent; const auto oldOriginal=g_originalTerrainBind;
    H=host; g_terrainCacheSpacing=spacing; g_getTerrainComponent=getter; g_originalTerrainBind=original;
    TerrainBindDetour(self,entity);
    H=oldHost; g_terrainCacheSpacing=oldSpacing; g_getTerrainComponent=oldGetter; g_originalTerrainBind=oldOriginal;
}
