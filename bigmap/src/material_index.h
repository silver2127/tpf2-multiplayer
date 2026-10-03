// Steam 35924 MaterialIndexManager pixel selection at RVA 0x315f20.
// Loop interchange only: retain 8-step batches with their 9-layer overlap,
// overlay/mask precedence, 0xe9 sentinel and exact scalar interpolation order.
#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
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

// ---- THE PROBE (material_index_probe=1, measurement only, 2026-09-28) ----------
// Can the material index be cached in the sidecar like the terrain? Per tile
// (the `tile` argument), each computed rectangle is copied aside; once all
// 65,536 pixels are in, the tile's run-length size, zero-order entropy and
// FNV-1a hash go to <data>/material_probe.txt. Two loads of one save give the
// same hashes if the output is a pure function of the save; the sizes say what
// a sidecar of it would cost.
static bool g_materialProbe = false;
namespace MaterialProbe {
struct Acc { std::vector<uint8_t> px; std::vector<uint8_t> seen; uint32_t filled = 0; };
static std::mutex m;
static std::unordered_map<uint64_t, Acc>* open = nullptr;
static FILE* out = nullptr;
static uint64_t tiles = 0, rleBytes = 0, calls = 0, pixels = 0, recomputed = 0;
static double entropyBits = 0;
static void Finish(uint64_t key, const uint8_t* px) {
    uint64_t runs = 0; uint32_t hist[256] = {};
    for (int i = 0; i < 65536; ++i) { ++hist[px[i]]; if (!i || px[i] != px[i - 1]) ++runs; }
    double bits = 0;
    for (uint32_t c : hist) if (c) { const double p = c / 65536.0; bits -= c * log2(p); }
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 65536; ++i) { h ^= px[i]; h *= 0x100000001b3ull; }
    ++tiles; rleBytes += runs * 2; entropyBits += bits;
    if (out) fprintf(out, "%d %d %016llx %llu %.0f\n", int32_t(key), int32_t(key >> 32), (unsigned long long)h, (unsigned long long)(runs * 2), bits / 8);
    if (tiles % 2000 == 0 && H)
        H->log("material probe: %llu tiles complete, %llu calls, %.2f pixels computed per tile pixel; raw %.0f MiB, run-length %.1f MiB, entropy %.1f MiB",
               (unsigned long long)tiles, (unsigned long long)calls, double(pixels) / (double(tiles) * 65536.0), tiles * 65536.0 / 1048576.0,
               rleBytes / 1048576.0, entropyBits / 8 / 1048576.0);
}
static void Record(uint64_t tile, int x0, int y0, int w, int h, const uint8_t* output) {
    std::lock_guard<std::mutex> l(m);
    if (!open) {
        open = new std::unordered_map<uint64_t, Acc>;
        if (H && H->dataDir) { std::string p = std::string(H->dataDir()) + "material_probe.txt"; out = fopen(p.c_str(), "w"); }
    }
    ++calls; pixels += uint64_t(w) * h;
    Acc& a = (*open)[tile];
    if (a.px.empty()) { a.px.assign(65536, 0); a.seen.assign(65536, 0); }
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) {
            const int p = y * 256 + x;
            a.px[p] = output[p];
            if (!a.seen[p]) { a.seen[p] = 1; ++a.filled; } else ++recomputed;
        }
    if (a.filled == 65536) {
        Finish(tile, a.px.data());
        if (out) fflush(out);
        open->erase(tile);
    }
}
}  // namespace MaterialProbe

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
    // Each layer's height map and material ID, read once per call instead of
    // through two dependent loads per layer per pixel (2026-09-28 profile: this
    // function was the load's largest plugin cost, ~46 CPU-s on a 36,992-tile
    // map). The layers do not change during a call: the stock code reads them
    // as fixed inputs too.
    constexpr int MaxLayers=256;
    if (count>MaxLayers) {
        g_originalMaterialIndex(block,tile,job,origin,overlay,layers,cell,baseVector,outputVector);
        return;
    }
    const float* layerHeights[MaxLayers];
    uint8_t layerId[MaxLayers];
    for (int k=0;k<count;++k) {
        const uint8_t* e=entries+size_t(k)*24;
        layerHeights[k]=reinterpret_cast<const float*>((*reinterpret_cast<const uintptr_t* const*>(e))[0]);
        layerId[k]=e[12];
    }
    const bool hasOverlay=overlay[0]!=overlay[1];
    // The dither column (dx*256+x)%63, stepped along the row instead of a 64-bit
    // division per pixel (dx and x are non-negative: checked above).
    const int ditherCol0=int((dx*256+x0)%63);
    for (int y=y0;y<y0+h;++y) {
        const float fy=float(y)*0.25f;
        const int iy=int(fy);
        const float ty=fy-float(iy);
        const float* dither=g_materialDither+int((dy*256+y)%63)*63;
        int ditherCol=ditherCol0;
        const int rowIndex=cell[0]*64+1+(iy+cell[1]*64+1)*stride;
        for (int x=x0;x<x0+w;++x,ditherCol=ditherCol==62?0:ditherCol+1) {
            const int pixel=y*256+x;
            const uint8_t b=base[(y-y0)*w+x-x0];
            uint8_t value=0xe9;
            bool evaluate=false;
            if (hasOverlay && extra[pixel]!=0 && b!=0xff) {
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
                const int index=rowIndex+ix;
                const float threshold=dither[ditherCol];
                for (int batch=0;batch<count;batch+=8) {
                    if (batch ? value!=0xe9 : !evaluate) continue;
                    int last=count-batch-9;
                    if (last<0) last=0;
                    bool found=false;
                    for (int k=count-batch-1;k>=last;--k) {
                        const float* heights=layerHeights[k];
                        const float top=(heights[index+1]-heights[index])*tx+heights[index];
                        const float bottom=(heights[index+stride+1]-heights[index+stride])*tx+heights[index+stride];
                        if (threshold < (bottom-top)*ty+top) {
                            value=layerId[k]; found=true; break;
                        }
                    }
                    if (!found && count<=batch+8) value=fallback;
                    if (value!=0xe9) break;
                }
            }
            output[pixel]=value;
        }
    }
    if (g_materialProbe) MaterialProbe::Record(tile,x0,y0,w,h,output);
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
