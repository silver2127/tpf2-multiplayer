// Native Steam Linux build 35924. See docs/linux/PORT.md for measured sites.
#include "../../native/src/plugin/tpf2mp_plugin.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <sys/mman.h>
#include "density.h"
#include "terrain_pager.h"
#include "alignment_batch.h"
#include "memory_budget.h"
#include "octree_depth.h"

extern "C" void TerrainMinMaxBridge();
extern "C" void* bigmap_minmax_return;
namespace {
const Tpf2mpHost* H;
constexpr const char* Section = "tpf2_bigmap";
constexpr uintptr_t SizeRva = 0x11232c0, RasterRva = 0x14e05a0;
constexpr uintptr_t ComboRva = 0x31443a0, ComboCall = 0x1149945;
constexpr uintptr_t RatioCall = 0x1149ae7, RatioRva = 0x14276c0;
constexpr uintptr_t OctreeSite = 0xa84234;
constexpr uintptr_t RasterCalls[] = {0x1066d80,0x150af14,0x151e175,0x15269cf};
constexpr uint8_t SizeBytes[] = {0x55,0x48,0x89,0xe5,0x41,0x55,0x41,0x89,0xf5,0x41,0x54,0x53,0x89,0xfb,0x48,0x83,0xec,0x58};
constexpr uint8_t OctreeBytes[] = {0xf3,0x0f,0x10,0x05,0x48,0x7c,0x40,0x03,0xbe,0x0a,0,0,0};
constexpr uint8_t RatioLoop[] = {0x83,0xfb,0x05};
constexpr uint8_t RatioGate[] = {0x83,0xfb,0x02};
// Compact-ID counters for depth 12/13 (level 11, level 12). Only the child
// stub writes them, with lock xadd; never reset or recycled in-process.
alignas(8) uint32_t octreeNext[2]={linux_octree::CounterStart[0],linux_octree::CounterStart[1]};
using SizeFn = uint64_t (*)(int,int,void*);
using RasterFn = void (*)(void*,const float*,float);
using ComboFn = void* (*)(const void*,void*,int,void*,int);
SizeFn originalSize;
int cap=512, maxRatio=20, rows=0;
std::atomic<int> stockRows{7};
double cellBudget=1.5e9;
struct Row { int side,index; char label[64]; };
Row extra[12];
struct Claim { int size,format,x,y; };
Claim claims[19*20]; int claimCount=0;
int tilesX=0,tilesY=0,sizeIndex=6,formatIndex=0;

uint64_t Pack(int x,int y) { return uint32_t(x)|(uint64_t(uint32_t(y))<<32); }
bool Fits(int x,int y) {
    // Until the Linux placement-distance implementation is widened, keep its
    // heightmap-coordinate diagonal squared inside int32 too (512^2 is just over).
    const int64_t px=int64_t(x)*64,py=int64_t(y)*64;
    return (px+1)*(py+1)<=INT_MAX && px*px+py*py<=INT_MAX;
}
void Bound(int& x,int& y) {
    x=std::clamp(x,2,cap)&~1; y=std::clamp(y,2,cap)&~1;
    while (!Fits(x,y)) { if(x>=y)x-=2;else y-=2; }
}
uint64_t Shape(int side,int format) {
    int ratio=std::clamp(format+1,1,std::min(maxRatio,cap/2));
    int x=std::max(2,2*int(std::floor(side/std::sqrt(double(ratio))/2+0.5)));
    x=std::min(x,2*((cap/ratio)/2));
    while(x>2 && !Fits(x,x*ratio))x-=2;
    return Pack(x,x*ratio);
}
bool Parse(const char* s,int& x,int& y) {
    if(!s || !*s)return false;
    char* end; long a=std::strtol(s,&end,10);
    if(end==s || a<2 || a>INT_MAX)return false;
    while(*end==' ' || *end=='\t')++end;
    if(*end!='x' && *end!='X')return false;
    const char* second=end+1;long b=std::strtol(second,&end,10);
    if(end==second || b<2 || b>INT_MAX)return false;
    while(*end==' ' || *end=='\t' || *end=='\n' || *end=='\r')++end;
    if(*end)return false;
    x=int(a);y=int(b);return true;
}
uint64_t Size(int size,int format,void* cfg) {
    size=std::max(size,0);format=std::clamp(format,0,maxRatio-1);
    const int row=size-stockRows.load();
    if(rows && row>=0 && row<rows) {
        for(int i=0;i<claimCount;++i)if(claims[i].size==extra[row].index && claims[i].format==format)
            return Pack(claims[i].x,claims[i].y);
        return Shape(extra[row].side,format);
    }
    for(int i=0;i<claimCount;++i)
        if(claims[i].size==size && claims[i].format==format)return Pack(claims[i].x,claims[i].y);
    if(size==sizeIndex && format==formatIndex && tilesX && tilesY)return Pack(tilesX,tilesY);
    if(size>=7)return Shape(96,format);
    if(format>=5) {
        const uint64_t square=originalSize(size,0,cfg);
        if(cfg) { int32_t xy[2];std::memcpy(xy,static_cast<uint8_t*>(cfg)+0x28,8);
            if(xy[0]>0 && xy[1]>0)return square; }
        return Shape(int(std::sqrt(double(uint32_t(square))*uint32_t(square>>32))),format);
    }
    return originalSize(size,format,cfg);
}
float RasterCell(float w,float h,float cell) {
    if(!std::isfinite(w) || !std::isfinite(h) || w<=0 || h<=0 || !std::isfinite(cell) || cell<=0)return cell;
    // Match the game's float division/truncation, using double for the product.
    auto cells=[&](float c){return (std::floor(double(w/c))+1)*(std::floor(double(h/c))+1);};
    while(cells(cell)>cellBudget)cell+=1.f;
    return cell;
}
void Raster(void* self,const float* box,float cell) {
    const float grown=RasterCell(box[2]-box[0],box[3]-box[1],cell);
    if(grown!=cell)H->log("street raster: %.0f -> %.0f m cells",double(cell),double(grown));
    reinterpret_cast<RasterFn>(H->moduleBase()+RasterRva)(self,box,grown);
}
// Borrow libstdc++ string storage; the game copies labels into its widget.
// No allocator or ownership crosses the ABI. Stock SSO pointers remain valid.
struct String { const char* data; size_t size; char local[16]; };
struct Vector { const String* begin; const String* end; const String* capacity; };
static_assert(sizeof(String)==32 && sizeof(Vector)==24,"libstdc++ x86-64 ABI");
void* Combo(const Vector* items,void* change,int selection,void* aux,int flags) {
    const auto original=reinterpret_cast<ComboFn>(H->moduleBase()+ComboRva);
    const size_t n=items->end-items->begin;
    if(n!=4 && n!=7) { H->log("unexpected stock size row count %zu; extra rows disabled",n);rows=0;return original(items,change,selection,aux,flags); }
    stockRows=int(n);
    String combined[32]{};
    std::memcpy(combined,items->begin,n*sizeof(String));
    for(int i=0;i<rows;++i){combined[n+i].data=extra[i].label;combined[n+i].size=std::strlen(extra[i].label);}
    Vector view{combined,combined+n+rows,combined+n+rows};
    H->log("size dropdown: %zu stock + %d native Linux rows",n,rows);
    return original(&view,change,selection,aux,flags);
}
String* RatioText(String* out,int format) {
    std::memset(out,0,sizeof(*out));out->data=out->local;
    out->size=std::snprintf(out->local,sizeof(out->local),"1:%d",std::clamp(format+1,1,maxRatio));
    return out;
}
void Jump(uint8_t* p,uintptr_t dest) {
    p[0]=0xff;p[1]=0x25;std::memset(p+2,0,4);std::memcpy(p+6,&dest,8);
}
void* Near(uintptr_t anchor) {
    for(uintptr_t d=0x100000;d<0x70000000;d+=0x100000)for(int sign:{1,-1}) {
        const uintptr_t at=(anchor+sign*d)&~uintptr_t(4095);
        void* p=mmap(reinterpret_cast<void*>(at),4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if(p==MAP_FAILED)continue;
        if(uintptr_t(p)==at)return p;
        munmap(p,4096);
    }
    return nullptr;
}
uint8_t* TownStub(uint8_t* page,uintptr_t ret) {
    uint8_t code[]={0x50,0x48,0xba,0,0,0,0,0,0,0,0, // push rax; movabs rdx,table (rdx saved below)
      0x83,0xf8,0x09,0x76,0x05,0xb8,0x0a,0,0,0,
      0xf3,0x0f,0x10,0x1c,0x82,0x58,0x5a,
      0xf3,0x0f,0x11,0x9d,0x44,0xfe,0xff,0xff,
      0xff,0x25,0,0,0,0,0,0,0,0,0,0,0,0};
    // Entry pushes rdx then rax; unsigned range check also covers negative indices.
    uint8_t* entry=page+512;*entry=0x52;
    const uintptr_t table=uintptr_t(page+3000);
    std::memcpy(code+3,&table,8);std::memcpy(code+42,&ret,8);
    std::memcpy(entry+1,code,sizeof(code));
    float values[11]={.2f,.3f,.4f,.5f};
    for(int i=0;i<6;++i)values[4+i]=float(.3*density::scales[i]);
    values[10]=1.f;
    std::memcpy(page+3000,values,sizeof(values));
    return entry;
}
size_t alignmentBatch=0;
void AlignmentUpdate(void* self,const linux_alignment::Map* map) {
    const auto update=reinterpret_cast<linux_alignment::Update>(H->moduleBase()+0x173dae0);
    const auto next=reinterpret_cast<linux_alignment::Increment>(H->moduleBase()+0x6dc1c0);
    const size_t n=linux_alignment::Run(self,map,alignmentBatch,update,next);
    if(n)H->log("alignment batch: %zu blocks in %zu batches of %zu",n,(n+alignmentBatch-1)/alignmentBatch,alignmentBatch);
}
linux_pager::TerrainPager* terrainPager=nullptr;
struct TerrainVector {uint16_t *begin,*end,*capacity;};
void TerrainAppend(TerrainVector* v,size_t n) {
    using Append=void(*)(TerrainVector*,size_t);
    const auto original=reinterpret_cast<Append>(H->moduleBase()+0xadb7e0);
    if(terrainPager->Contains(v->begin)) {
        const size_t size=v->end-v->begin;
        if(n<=TerrainCodec::Samples-size){std::memset(v->end,0,n*2);v->end+=n;return;}
        TerrainVector replacement{};original(&replacement,size+n);
        std::memcpy(replacement.begin,v->begin,size*2);terrainPager->Release(v->begin);*v=replacement;return;
    }
    if(!v->begin && !v->end && !v->capacity && n==TerrainCodec::Samples) {
        if(auto p=terrainPager->Allocate()){*v={p,p+n,p+n};return;}
    }
    original(v,n);
}
void* TerrainCopyAllocate(size_t bytes) {
    if(bytes==TerrainCodec::RawBytes)if(auto p=terrainPager->Allocate())return p;
    return reinterpret_cast<void*(*)(size_t)>(H->moduleBase()+0x6dbce0)(bytes);
}
void TerrainDispose(void* control) {
    auto* v=reinterpret_cast<TerrainVector*>(static_cast<uint8_t*>(control)+16);
    if(terrainPager->Release(v->begin)){*v={};return;}
    if(v->begin)reinterpret_cast<void(*)(void*)>(H->moduleBase()+0x6dbcd0)(v->begin);
}
struct Patch { uintptr_t rva; uint8_t before[128]{},after[128]{}; uint32_t len; };
Patch patches[32];int patchCount=0;
bool Plan(uintptr_t rva,const uint8_t* before,const uint8_t* after,uint32_t len) {
    if(patchCount>=32 || len>128 || !H->verifyBytes(rva,before,len))return false;
    auto& p=patches[patchCount++];p.rva=rva;p.len=len;
    std::memcpy(p.before,before,len);std::memcpy(p.after,after,len);return true;
}
bool PlanCall(uintptr_t rva,uintptr_t callee,void* target,uint8_t*& stub) {
    uint8_t before[5]={0xe8},after[5]={0xe8};
    int32_t a=int32_t(callee-rva-5),b=int32_t(uintptr_t(stub)-(H->moduleBase()+rva+5));
    std::memcpy(before+1,&a,4);std::memcpy(after+1,&b,4);Jump(stub,uintptr_t(target));stub+=16;
    return Plan(rva,before,after,5);
}
}

extern "C" __attribute__((visibility("default")))
int Tpf2mpPluginInit(const Tpf2mpHost* host,Tpf2mpPluginInfo* info) {
    if(!host || !info || host->abiMajor!=TPF2MP_ABI_MAJOR || host->size<sizeof(Tpf2mpHost))return TPF2MP_ERR_ABI;
    H=host;*info={"tpf2_bigmap","0.4.0-linux-dev.3","Native Linux large maps, sparse density and lossless terrain paging"};
    const auto baseMod=density::GamePath();
    std::string densityWhy;
    // Remove our prior labels before validating hooks, so a failed initialization
    // cannot expose unsupported town indices on this run.
    const bool restored=density::Sync(baseMod,false,densityWhy);
    if(!H->buildOk() || !H->moduleBase())return TPF2MP_ERR_BUILD;
    const bool enabled=H->cfgBool(Section,"enabled",1);
    const bool sparse=enabled && H->cfgBool(Section,"newgame_density",1);
    if(sparse && !restored){H->log("density restore failed: %s",densityWhy.c_str());return TPF2MP_ERR_FAILED;}
    if(!enabled)return TPF2MP_ERR_DISABLED;
    const bool octree=H->cfgBool(Section,"octree",1),raster=H->cfgBool(Section,"street_raster",1);
    const int depth=octree?H->cfgInt(Section,"octree_depth",11):11;
    if(!linux_octree::ValidDepth(depth)){H->log("octree_depth must be 11, 12 or 13; refusing unsupported depth %d",depth);return TPF2MP_ERR_FAILED;}
    cap=std::clamp(H->cfgInt(Section,"max_tiles",512),2,linux_octree::EdgeTiles(depth))&~1;
    maxRatio=std::clamp(H->cfgInt(Section,"max_ratio",20),5,20);
    cellBudget=double(std::clamp(H->cfgInt(Section,"cell_budget_millions",1500),1,2000))*1e6;
    if(!octree)cap=std::min(cap,256);
    if(!raster)cap=std::min(cap,180); // never offer overflowing generation
    tilesX=H->cfgInt(Section,"tiles_x",0);tilesY=H->cfgInt(Section,"tiles_y",0);
    if(tilesX>0 && tilesY>0)Bound(tilesX,tilesY);else tilesX=tilesY=0;
    sizeIndex=H->cfgInt(Section,"size_index",6);formatIndex=H->cfgInt(Section,"format_index",0);
    for(int s=0;s<19;++s)for(int f=0;f<20;++f) {
        char key[32];std::snprintf(key,sizeof(key),"size%d_format%d",s,f);int x,y;
        if(Parse(H->cfgStr(Section,key,""),x,y)){Bound(x,y);claims[claimCount++]={s,f,x,y};}
    }
    const int defaults[]={128,160,192,224,256,320,384,448,512};
    if(H->cfgBool(Section,"add_size_rows",1))for(int index=7;index<19;++index) {
        int side=index<16?defaults[index-7]:0;
        for(int i=0;i<claimCount;++i)if(claims[i].size==index && claims[i].format==0)
            side=int(std::sqrt(double(claims[i].x)*claims[i].y));
        if(!side || side>cap)continue;
        auto& row=extra[rows++];row.side=side;row.index=index;
        char key[32],fallback[64];std::snprintf(key,sizeof(key),"size_label%d",index);
        const auto square=Shape(side,0);
        std::snprintf(fallback,sizeof(fallback),"%.2f x %.2f km",uint32_t(square)*.256,uint32_t(square>>32)*.256);
        std::snprintf(row.label,sizeof(row.label),"%s",H->cfgStr(Section,key,fallback));
    }
    if(!H->verifyBytes(SizeRva,SizeBytes,sizeof(SizeBytes)))return TPF2MP_ERR_BUILD;
    auto* page=static_cast<uint8_t*>(Near(H->moduleBase()+SizeRva));if(!page)return TPF2MP_ERR_FAILED;
    uint8_t* stub=page;
    // A relocated constant keeps the stock instruction shape and register ABI.
    const float extent=linux_octree::RootHalfExtent(octree?depth:11);std::memcpy(page+4000,&extent,4);
    if(octree && depth>=12) {
        // Preflight both ID sites before the root; they are published first and
        // the root/depth change last. Any mismatch refuses with nothing written.
        using namespace linux_octree;
        uint8_t child[sizeof(ChildBytes)],level[sizeof(LevelBytes)];
        BuildLevelStub(page+1024,H->moduleBase()+LevelBack);
        BuildChildStub(page+1280,H->moduleBase()+ChildBack,octreeNext);
        SitePatch(level,sizeof(level),uintptr_t(page+1024));
        SitePatch(child,sizeof(child),uintptr_t(page+1280));
        if(!Plan(ChildSite,ChildBytes,child,sizeof(child)) || !Plan(LevelSite,LevelBytes,level,sizeof(level))) {
            H->log("octree: depth %d byte mismatch; refused, nothing patched",depth);return TPF2MP_ERR_BUILD;
        }
    }
    if(octree) {
        uint8_t patch[13];std::memcpy(patch,OctreeBytes,13);
        int32_t rel=int32_t(uintptr_t(page+4000)-(H->moduleBase()+OctreeSite+8));
        std::memcpy(patch+4,&rel,4);patch[9]=uint8_t(depth);
        if(!Plan(OctreeSite,OctreeBytes,patch,13))return TPF2MP_ERR_BUILD;
    }
    if(raster)for(auto call:RasterCalls)if(!PlanCall(call,RasterRva,reinterpret_cast<void*>(Raster),stub))return TPF2MP_ERR_BUILD;
    if(rows && !PlanCall(ComboCall,ComboRva,reinterpret_cast<void*>(Combo),stub))return TPF2MP_ERR_BUILD;
    if(maxRatio>5) {
        uint8_t loop[3]={0x83,0xfb,uint8_t(maxRatio)},gate[3]={0x83,0xfb,uint8_t(maxRatio-1)};
        if(!PlanCall(RatioCall,RatioRva,reinterpret_cast<void*>(RatioText),stub) ||
           !Plan(0x1149bf4,RatioLoop,loop,3) || !Plan(0x1149c01,RatioGate,gate,3))return TPF2MP_ERR_BUILD;
    }
    if(sparse) {
        // eax is the selected Towns index; cases 0..2 have already branched.
        // Preserve every register except xmm3, which the stock default writes.
        const uint8_t head[]={0x48,0x8b,0x43,0x18,0x8b,0x80,0x60,0x04,0,0};
        if(!H->verifyBytes(0x112e2ef,head,sizeof(head)))return TPF2MP_ERR_BUILD;
        constexpr uintptr_t site=0x112e313, back=0x112e32c;
        const uint8_t before[]={0xf3,0x0f,0x10,0x1d,0xad,0xda,0xd5,0x02,0xf3,0x0f,0x11,0x9d,0x44,0xfe,0xff,0xff,0x83,0xf8,0x03,0x0f,0x84,0x74,0x04,0,0};
        uint8_t* entry=TownStub(page,H->moduleBase()+back);
        uint8_t after[sizeof(before)];std::memset(after,0x90,sizeof(after));Jump(after,uintptr_t(entry));
        if(!Plan(site,before,after,sizeof(before)))return TPF2MP_ERR_BUILD;
    }
    // Experimental until a live load proves the publication/lifetime contract.
    alignmentBatch=size_t(std::clamp(H->cfgInt(Section,"alignment_batch_tiles",0),0,65536));
    if(alignmentBatch) {
        const uint8_t entry[]={0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x49,0x89,0xfe,0x41,0x55,0x41,0x54,0x49,0x89,0xf4};
        const uint8_t iter[]={0x4c,0x89,0xff,0xe8,0xa3,0xe1,0xf9,0xfe,0x49,0x89,0xc7};
        const uint8_t value[]={0x49,0x8d,0x47,0x28,0x49,0x8b,0x76,0x08,0x4d,0x8d,0x46,0x20};
        const uint8_t head[]={0x4d,0x8b,0x7c,0x24,0x18,0x49,0x8d,0x7c,0x24,0x08};
        const uint8_t caller[]={0x4c,0x8d,0xa7,0xa0,0,0,0,0x53,0x48,0x83,0xbf,0xc8,0,0,0,0};
        const uint8_t publish[]={0x49,0x8b,0x7e,0x08,0x4c,0x89,0xee,0xe8,0x5c,0x75,0x5b,0xff};
        if(!H->verifyBytes(0x173dae0,entry,sizeof(entry)) ||
           !H->verifyBytes(0x173e015,iter,sizeof(iter)) ||
           !H->verifyBytes(0x173df1f,value,sizeof(value)) ||
           !H->verifyBytes(0x173db87,head,sizeof(head)) ||
           !H->verifyBytes(0x173e3ea,caller,sizeof(caller)) ||
           !H->verifyBytes(0x173e168,publish,sizeof(publish)) ||
           !PlanCall(0x173e443,0x173dae0,reinterpret_cast<void*>(AlignmentUpdate),stub)) {
            alignmentBatch=0;H->log("alignment batch: byte mismatch; stock path retained");
        }
    }
    const bool minMax=H->cfgBool(Section,"terrain_minmax_fast",1);
    if(minMax) {
        const uint8_t before[]={0xf,0xb7,0x13,0x48,0x83,0xc3,0x2,0xf,0xb7,0xc2,0x41,0x89,0xd5,0xeb,0x16,0xf,0x1f,0x80,0x0,0x0,0x0,0x0,0x44,0xf,0xb7,0x3b,0x41,0x89,0xc5,0x48,0x83,0xc3,0x2,0x41,0xf,0xb7,0xc7,0x66,0x44,0x39,0xe8,0x72,0xa,0x66,0x39,0xc2,0xf,0x42,0xd0,0x41,0xf,0xb7,0xc5,0x49,0x39,0xde,0x75,0xdc};
        uint8_t after[sizeof(before)];std::memset(after,0x90,sizeof(after));Jump(after,uintptr_t(TerrainMinMaxBridge));
        bigmap_minmax_return=reinterpret_cast<void*>(H->moduleBase()+0xcf588c);
        if(!Plan(0xcf5852,before,after,sizeof(before)))return TPF2MP_ERR_BUILD;
    }
    const bool fastSave=H->cfgBool(Section,"save_fast",1);
    if(fastSave) {
        const uint8_t levelBefore[]={0x8b,0x05,0x9e,0x27,0x3c,0x04},levelAfter[]={0xb8,1,0,0,0,0x90};
        const uint8_t cmpBefore[]={0x49,0x81,0x7c,0x24,0x70,0x80,0,0,0},cmpAfter[]={0x49,0x81,0x7c,0x24,0x70,0,0,1,0};
        const uint8_t allocBefore[]={0xbf,0x80,0,0,0},allocAfter[]={0xbf,0,0,1,0};
        const uint8_t sizeBefore[]={0x49,0xc7,0x44,0x24,0x70,0x80,0,0,0},sizeAfter[]={0x49,0xc7,0x44,0x24,0x70,0,0,1,0};
        if(!Plan(0xc7b524,levelBefore,levelAfter,6) || !Plan(0xc7b6b1,cmpBefore,cmpAfter,9) ||
           !Plan(0xc7c3a0,allocBefore,allocAfter,5) || !Plan(0xc7c3aa,sizeBefore,sizeAfter,9))return TPF2MP_ERR_BUILD;
    }
    if(H->cfgBool(Section,"terrain_cache_compress",1)) {
        // Scope allocation changes to CTerrain's append and detached-copy calls.
        // Dispose is the shared-vector control block's exact native free path.
        auto* candidate=new linux_pager::TerrainPager;
        const int configuredHot=H->cfgInt(Section,"terrain_cache_hot_mb",0);
        const int hot=linux_memory::TerrainHotMB(linux_memory::PhysicalBytes(),configuredHot);
        H->log("terrain compression: startup resident budget %d MiB (%s)",hot,configuredHot>0?"fixed":"automatic headroom");
        if(!candidate->Start(1u<<20,size_t(hot)<<20,configuredHot<=0)) {
            delete candidate;H->log("terrain compression unavailable: userfaultfd missing/write-protect support required");
        } else {
            terrainPager=candidate;
            const uint8_t before[]={0xf3,0x0f,0x1e,0xfa,0x48,0x8b,0x7f,0x10,0x48,0x85,0xff,0x74,0x0b,0xe9,0x0e,0x41,0x9e,0xff};
            uint8_t after[sizeof(before)];std::memset(after,0x90,sizeof(after));Jump(after,uintptr_t(TerrainDispose));
            if(!Plan(0xcf7bb0,before,after,sizeof(before)) ||
               !PlanCall(0xcf7696,0xadb7e0,reinterpret_cast<void*>(TerrainAppend),stub) ||
               !PlanCall(0xcf7783,0x6dbce0,reinterpret_cast<void*>(TerrainCopyAllocate),stub))return TPF2MP_ERR_BUILD;
        }
    }
    if(mprotect(page,4096,PROT_READ|PROT_EXEC))return TPF2MP_ERR_FAILED;
    if(sparse && (!density::Write(std::string(H->dataDir())+"/bigmap-base-mod.path",baseMod+"\n") || !density::Sync(baseMod,true,densityWhy))) {
        H->log("density levels refused: %s",densityWhy.c_str());return TPF2MP_ERR_FAILED;
    }
    void* trampoline=nullptr;
    if(!H->installHook(H->moduleBase()+SizeRva,reinterpret_cast<void*>(Size),sizeof(SizeBytes),&trampoline)){if(sparse)density::Sync(baseMod,false,densityWhy);return TPF2MP_ERR_FAILED;}
    originalSize=reinterpret_cast<SizeFn>(trampoline);
    for(int i=0;i<patchCount;++i)if(!H->patchBytes(patches[i].rva,patches[i].after,patches[i].len)) {
        // Keep code and original trampoline mapped even after rollback.
        for(int j=i;j>=0;--j)H->patchBytes(patches[j].rva,patches[j].before,patches[j].len);
        H->patchBytes(SizeRva,SizeBytes,sizeof(SizeBytes));
        if(sparse)density::Sync(baseMod,false,densityWhy);
        H->log("patch failed; attempted rollback, restart before using big maps");return TPF2MP_ERR_FAILED;
    }
    if(sparse){
        H->log("density levels active: six sparse presets for towns, industries and industry target");
    }
    if(minMax)H->log("terrain min/max: exact SSE2 scan enabled");
    if(fastSave)H->log("fast saves: zstd level 1, 64 KiB input buffer; save files may be larger");
    if(terrainPager){
        H->log("terrain compression: Linux userfaultfd, lossless 1 m codec, enabled");
        std::thread([]{
            auto previous=std::chrono::steady_clock::now();uint64_t previousFaults=0;
            for(;;){
                std::this_thread::sleep_for(std::chrono::seconds(30));
                const auto now=std::chrono::steady_clock::now();auto s=terrainPager->Get();
                const double seconds=std::chrono::duration<double>(now-previous).count();
                H->log("terrain pager: live=%llu resident=%.1f MiB packed=%.1f MiB budget=%.1f MiB faults=%llu faults/s=%.1f evictions=%llu refusals=%llu",
                    (unsigned long long)s.live,s.resident*linux_pager::TerrainPager::Stride/1048576.,
                    s.packed/1048576.,s.budget/1048576.,(unsigned long long)s.faults,
                    (s.faults-previousFaults)/seconds,(unsigned long long)s.evictions,(unsigned long long)s.refusals);
                previous=now;previousFaults=s.faults;
            }
        }).detach();
    }
    H->log("Linux map controls active: %d-tile edge cap, depth %d, %d added sizes, ratios 1:1..1:%d",cap,octree?depth:10,rows,maxRatio);
    if(octree && depth>=12)H->log("octree: EXPERIMENTAL depth %d, root +-%.0f m, 128 m leaves; %d-tile edge capacity; compact IDs enabled",
        depth,double(linux_octree::RootHalfExtent(depth)),linux_octree::EdgeTiles(depth));
    H->log("Experimental port: material compression is not enabled");
    return TPF2MP_OK;
}
