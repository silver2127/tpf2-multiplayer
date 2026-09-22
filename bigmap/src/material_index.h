// Steam 35924 MaterialIndexManager pixel selection at RVA 0x315f20.
// Loop interchange only: retain 8-step batches with their 9-layer overlap,
// overlay/mask precedence, 0xe9 sentinel and exact scalar interpolation order.
#pragma once
using MaterialIndexFn = void (__fastcall*)(uint64_t,uint64_t,uint64_t,uint64_t,
    const uintptr_t*,const uint8_t*,const int32_t*,const uintptr_t*,const uintptr_t*);
static MaterialIndexFn g_originalMaterialIndex = nullptr;
static const float* g_materialDither = nullptr;
static bool g_materialIndexFast = false;
static int32_t MaterialLo(uint64_t a) { return int32_t(a); }
static int32_t MaterialHi(uint64_t a) { return int32_t(a >> 32); }
static bool MaterialOverlap(uintptr_t a,size_t an,uintptr_t b,size_t bn) {
    return an && bn && (a<b ? b-a<an : a-b<bn);
}

static void __fastcall MaterialIndexDetour(uint64_t block,uint64_t tile,uint64_t job,
    uint64_t origin,const uintptr_t* overlay,const uint8_t* layers,const int32_t* cell,
    const uintptr_t* baseVector,const uintptr_t* outputVector)
{
    const int count=*reinterpret_cast<const int*>(layers+40);
    if (count <= 0) return; // original leaves output untouched
    const int w=MaterialLo(block), h=MaterialHi(block);
    const int jx=MaterialLo(job), jy=MaterialHi(job);
    const int64_t dx=int64_t(MaterialLo(tile))-MaterialLo(origin);
    const int64_t dy=int64_t(MaterialHi(tile))-MaterialHi(origin);
    // The measured path processes rectangular subregions of a 256x256 tile.
    // Preserve stock behavior for an unmeasured call geometry.
    if (w<=0 || h<=0 || w>256 || h>256 || jx<0 || jy<0 ||
        (int64_t(jx)+1)*w>256 || (int64_t(jy)+1)*h>256 ||
        dx<0 || dy<0 || dx>0x7fffff || dy>0x7fffff) {
        g_originalMaterialIndex(block,tile,job,origin,overlay,layers,cell,baseVector,outputVector);
        return;
    }
    if (MaterialOverlap(outputVector[0],65536,baseVector[0],size_t(w)*h) ||
        (overlay[0]!=overlay[1] &&
         (MaterialOverlap(outputVector[0],65536,overlay[0],65536) ||
          MaterialOverlap(outputVector[0],65536,overlay[3],8192)))) {
        g_originalMaterialIndex(block,tile,job,origin,overlay,layers,cell,baseVector,outputVector);
        return;
    }
    const int x0=jx*w,y0=jy*h;
    const auto entries=*reinterpret_cast<const uint8_t* const*>(layers);
    const int stride=*reinterpret_cast<const int*>(layers+36);
    const uint8_t fallback=layers[24];
    const auto base=reinterpret_cast<const uint8_t*>(baseVector[0]);
    const auto extra=reinterpret_cast<const uint8_t*>(overlay[0]);
    const auto mask=reinterpret_cast<const uint32_t*>(overlay[3]);
    auto output=reinterpret_cast<uint8_t*>(outputVector[0]);
    for (int y=y0;y<y0+h;++y) {
        const float fy=float(y)*0.25f;
        const int iy=int(fy);
        const float ty=fy-float(iy);
        const int ditherRow=int((dy*256+y)%63)*63;
        for (int x=x0;x<x0+w;++x) {
            const int pixel=y*256+x;
            const uint8_t b=base[(y-y0)*w+x-x0];
            uint8_t value=0xe9;
            bool evaluate=false;
            if (overlay[0]!=overlay[1] && extra[pixel]!=0 && b!=0xff) {
                value=extra[pixel];
                if (((mask[pixel>>5]>>(pixel&31))&1)==0 && b!=0) value=b;
            } else if (b!=0) value=b;
            else evaluate=true;
            // Calculate interpolation coordinates only for unresolved pixels.
            // A reserved 0xe9 value can become unresolved in a later batch;
            // preserve that behavior too instead of treating it as a final ID.
            if (evaluate || (value==0xe9 && count>8)) {
                const float fx=float(x)*0.25f;
                const int ix=int(fx);
                const float tx=fx-float(ix);
                const int index=cell[0]*64+1+ix+(iy+cell[1]*64+1)*stride;
                const float threshold=g_materialDither[ditherRow+int((dx*256+x)%63)];
                for (int batch=0;batch<count;batch+=8) {
                    if (batch ? value!=0xe9 : !evaluate) continue;
                    int last=count-batch-9;
                    if (last<0) last=0;
                    bool found=false;
                    for (int k=count-batch-1;k>=last;--k) {
                        const uint8_t* e=entries+size_t(k)*24;
                        const auto map=*reinterpret_cast<const uintptr_t* const*>(e);
                        const auto heights=reinterpret_cast<const float*>(map[0]);
                        const float top=(heights[index+1]-heights[index])*tx+heights[index];
                        const float bottom=(heights[index+stride+1]-heights[index+stride])*tx+heights[index+stride];
                        if (threshold < (bottom-top)*ty+top) {
                            value=e[12]; found=true; break;
                        }
                    }
                    if (!found && count<=batch+8) value=fallback;
                    if (value!=0xe9) break;
                }
            }
            output[pixel]=value;
        }
    }
}
static bool InstallMaterialIndexFast() {
    if (!g_materialIndexFast || g_gog) return false;
    const uint8_t expected[]={0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,
        0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    if (!H->verifyBytes(0x315f20,expected,sizeof expected)) {
        H->log("material index fast: byte mismatch; OFF"); return false;
    }
    g_materialDither=reinterpret_cast<const float*>(H->moduleBase()+0x2f87d20);
    void* original=nullptr;
    if (!H->installHook(H->moduleBase()+0x315f20,(void*)&MaterialIndexDetour,sizeof expected,&original)) {
        H->log("material index fast: hook failed; OFF"); return false;
    }
    g_originalMaterialIndex=reinterpret_cast<MaterialIndexFn>(original);
    H->log("material index fast: pixel-major texture selection enabled; resolution and material rules preserved");
    return true;
}
extern "C" __declspec(dllexport)
void BigmapTestMaterialIndex(MaterialIndexFn original,const float* dither,uint64_t a,uint64_t b,
    uint64_t c,uint64_t d,const uintptr_t* e,const uint8_t* f,const int32_t* g,
    const uintptr_t* h,const uintptr_t* i) {
    const auto oldOriginal=g_originalMaterialIndex; const auto oldDither=g_materialDither;
    g_originalMaterialIndex=original; g_materialDither=dither;
    MaterialIndexDetour(a,b,c,d,e,f,g,h,i);
    g_originalMaterialIndex=oldOriginal; g_materialDither=oldDither;
}
extern "C" __declspec(dllexport)
int BigmapTestInstallMaterialIndex(const Tpf2mpHost* host,int gog,int enabled) {
    const auto oldHost=H;const bool oldGog=g_gog,oldEnabled=g_materialIndexFast;
    H=host;g_gog=gog!=0;g_materialIndexFast=enabled!=0;
    const bool result=InstallMaterialIndexFast();
    H=oldHost;g_gog=oldGog;g_materialIndexFast=oldEnabled;
    return result;
}
