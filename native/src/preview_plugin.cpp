// Independent cosmetic BuilderRenderers for build 35924. See docs/BUILD_PREVIEWS.md.
// The GUI constructs a command solely to use scripting::Convert. It never sends it.
// This plugin reads the converted proposal, evaluates it and uploads preview buffers;
// it has no command-dispatch or applyProposal entry point.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include "plugin/tpf2mp_plugin.h"

namespace {
const Tpf2mpHost* host;
uintptr_t base;
void* scene;
DWORD guiThread;
uint64_t session;
alignas(16) unsigned char factory[0xd8];
bool factoryLive;
struct Peer {
    char origin[3]{}; void* renderer{}; uint64_t seen{};
    unsigned char originalPalette[0x80]{};
};
Peer peers[16];
using FactoryFn = void* (*)(void*);
using AddFn = void (*)(void*,void*);
using DtorFn = void (*)(void*);
using ConvertFn = void* (*)(void*,void*,void*);
FactoryFn originalFactory;
AddFn originalAdd;
DtorFn originalSceneDtor;
ConvertFn originalConvert;
using ClearFn = void (*)(void*,bool,bool);
using HeightFn = void (*)(void*,void*,bool);
ClearFn originalClear;
DtorFn originalEndHeight, originalRendererDtor;
// Height uploads are global to the UI terrain, unlike the model renderers.
// Local descriptors remain owned by their BuilderRenderer until Clear/destroy.
std::vector<void*> localHeightRenderers;
bool editingRemote, disposing, renderThreadLogged;
void* terrainTarget;
HeightFn uploadHeight;
using ResetHeightFn = void (*)(void*,bool);
ResetHeightFn resetHeight;
using ErrorColorFn = void (*)(void*,bool);
ErrorColorFn setErrorColor;
void* senderColorRenderer;
int senderErrorColor=-1;
void errorColor(void* renderer,bool invalid) {
    if(renderer==senderColorRenderer && senderErrorColor>=0) invalid=senderErrorColor!=0;
    setErrorColor(renderer,invalid);
}
// Only this peer's cosmetic renderer is changed. Legacy/unknown sender state
// retains the receiver's evaluation; errors/warnings in ProposalData stay intact.
void applySenderColor(void* renderer,const char* mode) {
    senderColorRenderer=renderer;
    senderErrorColor=!strcmp(mode,"drawok") ? 0 : (!strcmp(mode,"drawbad") ? 1 : -1);
}
template<class T> T at(uintptr_t rva) { return reinterpret_cast<T>(base+rva); }
template<class T> T& field(void* p, size_t offset) { return *reinterpret_cast<T*>(static_cast<char*>(p)+offset); }
void applySenderPalette(Peer& p,int invalid) {
    auto palette=static_cast<unsigned char*>(p.renderer)+0x118;
    memcpy(palette,p.originalPalette,sizeof(p.originalPalette));
    // Four matching RGBA pairs: per-segment error selection must use the
    // sender's palette as well as the renderer-wide/terrain error flag.
    if(invalid==0) memcpy(palette+0x40,p.originalPalette,0x40);
    else if(invalid==1) memcpy(palette,p.originalPalette+0x40,0x40);
}

bool active(const Peer& p) { return p.seen && GetTickCount64()-p.seen<=4000; }
bool isPeerRenderer(void* r) {
    for(const auto& p:peers) if(p.renderer==r) return true;
    return false;
}
bool hasRemoteTerrain() {
    for(const auto& p:peers) if(p.renderer && p.seen) return true;
    return false;
}
void uploadRendererHeight(void* r) {
    if(!r || field<void*>(r,0x50)!=terrainTarget || !field<bool>(r,0xf0)) return;
    auto state=field<unsigned char*>(r,0x1b8);
    if(field<void*>(state,0x16b0)!=field<void*>(state,0x16b8))
        uploadHeight(terrainTarget,state+0x16b0,field<bool>(r,0xf4));
}
void composeTerrain() {
    if(!terrainTarget || disposing) return;
    resetHeight(terrainTarget,true);
    // Remote uploads first; one's own tool keeps priority where areas overlap.
    for(const auto& p:peers) if(active(p)) uploadRendererHeight(p.renderer);
    for(void* r:localHeightRenderers) uploadRendererHeight(r);
}
void forgetLocalHeight(void* r) {
    localHeightRenderers.erase(std::remove(localHeightRenderers.begin(),localHeightRenderers.end(),r),localHeightRenderers.end());
}
void clearRenderer(void* r,bool models,bool terrain) {
    forgetLocalHeight(r);
    originalClear(r,models,terrain);
    if(!editingRemote && !disposing && scene && GetCurrentThreadId()==guiThread && hasRemoteTerrain())
        composeTerrain();
}
void endHeight(void* r) {
    const bool remote=isPeerRenderer(r);
    const bool enabled=field<bool>(r,0xf0);
    // AddHeightMod writes the error flag directly, bypassing the setter.
    // Restore sender colour before EndHeightMod bakes its tinted overlay.
    if(remote && r==senderColorRenderer && senderErrorColor>=0)
        errorColor(r,false);
    // Keep the native mesh generation, but publish remote terrain as a group.
    if(remote) field<bool>(r,0xf0)=false;
    originalEndHeight(r);
    if(remote) field<bool>(r,0xf0)=enabled;
    else if(scene && enabled && GetCurrentThreadId()==guiThread) {
        if(std::find(localHeightRenderers.begin(),localHeightRenderers.end(),r)==localHeightRenderers.end())
            localHeightRenderers.push_back(r);
    }
}
void destroyRenderer(void* r) {
    forgetLocalHeight(r);
    if(!disposing && !editingRemote && scene && GetCurrentThreadId()==guiThread && hasRemoteTerrain()) composeTerrain();
    originalRendererDtor(r);
}

std::string path(const char* name) { return std::string(host->dataDir())+"tpf2mp_preview_native_"+name+".txt"; }
void write(const char* name, const std::string& value) {
    FILE* f=nullptr;
    if(fopen_s(&f,path(name).c_str(),"wb") || !f) return;
    fwrite(value.data(),1,value.size(),f); fputs("\nend\n",f); fclose(f);
}
std::string readRequest() {
    FILE* f=nullptr;
    // Lua writes text files on Windows; normalize CRLF before checking the footer.
    if(fopen_s(&f,path("request").c_str(),"r") || !f) return {};
    char buf[192]{}; size_t n=fread(buf,1,sizeof(buf),f); fclose(f);
    if(n==sizeof(buf) || n<5 || memcmp(buf+n-5,"\nend\n",5)) return {};
    return std::string(buf,n-5);
}
void clear(Peer& p) {
    p.seen=0;
    if(p.renderer) originalClear(p.renderer,true,false);
}
bool expirePeers() {
    bool changed=false;
    for(auto& p:peers) if(p.seen && !active(p)) { clear(p); changed=true; }
    return changed;
}
// Native render passes take this, renderer component and render helper. Forward
// the fourth register too; short methods simply ignore the additional arguments.
using RenderFn = void (*)(void*,void*,void*,void*);
template<int Slot> void render(void* self,void* a,void* b,void* c) {
    if constexpr(Slot==1) {
        if(!renderThreadLogged) {
            host->log("[previews] render/GUI thread match=%d",GetCurrentThreadId()==guiThread);
            renderThreadLogged=true;
        }
        if(GetCurrentThreadId()==guiThread && expirePeers()) composeTerrain();
    }
    for(const auto& p:peers) if(p.renderer==self) {
        if(p.seen && GetTickCount64()-p.seen<=4000)
            reinterpret_cast<RenderFn*>(base+0x30665e0)[Slot](self,a,b,c);
        return;
    }
}
void* previewVtable[7];

void dispose() {
    disposing=true;
    if(terrainTarget) resetHeight(terrainTarget,true);
    localHeightRenderers.clear();
    for(auto& p:peers) {
        if(p.renderer) {
            if(scene) at<AddFn>(0x6d9290)(scene,p.renderer);
            clear(p);
            at<void*(*)(void*,unsigned)>(0x8163c0)(p.renderer,1);
        }
        p=Peer{};
    }
    scene=nullptr;
    terrainTarget=nullptr; disposing=false; renderThreadLogged=false;
    if(factoryLive) {
        void* callable=field<void*>(factory,0xb8);
        if(callable) reinterpret_cast<void(*)(void*,bool)>(field<void**>(callable,0)[4])
            (callable,callable!=factory+0x80);
        factoryLive=false;
    }
    write("ready","");
    host->log("[previews] scene disposed");
}
void* makeFactory(void* source) {
    void* result=originalFactory(source);
    // First street builder during CGameUI construction supplies a complete live
    // factory. Copy its native std::function using its own clone operation.
    if(!factoryLive && reinterpret_cast<uintptr_t>(_ReturnAddress())-base==0x445897) {
        memcpy(factory,source,sizeof(factory));
        field<void*>(factory,0xb8)=nullptr;
        void* callable=field<void*>(source,0xb8);
        if(callable) field<void*>(factory,0xb8)=
            reinterpret_cast<void*(*)(void*,void*)>(field<void**>(callable,0)[0])(callable,factory+0x80);
        factoryLive=true;
    }
    return result;
}
void addRenderable(void* target,void* object) {
    originalAdd(target,object);
    if(factoryLive && !scene && reinterpret_cast<uintptr_t>(_ReturnAddress())-base==0x56a532) {
        scene=target; guiThread=GetCurrentThreadId();
        session=(uint64_t(GetCurrentProcessId())<<32)^GetTickCount64();
        write("ready",std::to_string(session));
        host->log("[previews] independent renderer service ready");
    }
}
void destroyScene(void* target) {
    if(target==scene) dispose();
    originalSceneDtor(target);
}
size_t count(void* p,size_t offset,size_t stride) {
    auto begin=field<uintptr_t>(p,offset), end=field<uintptr_t>(p,offset+8);
    if(end<begin || (end-begin)%stride || (end-begin && !begin)) return SIZE_MAX;
    return (end-begin)/stride;
}
bool safeProposal(void* p) {
    const size_t nodes=count(p,0,24), edges=count(p,0x18,120);
    if(nodes>48 || edges>24 || (edges && !nodes)) return false;
    for(size_t off:{size_t(0x30),size_t(0x48),size_t(0xe0),size_t(0xf8),size_t(0x1e0),size_t(0x1f8)})
        if(field<void*>(p,off)!=field<void*>(p,off+8)) return false;
    auto ns=field<unsigned char*>(p,0), es=field<unsigned char*>(p,0x18);
    for(size_t i=0;i<nodes;i++) {
        auto n=ns+i*24;
        if(field<int>(n,0x14)>=0) return false;
        for(size_t j=0;j<3;j++) if(!std::isfinite(field<float>(n,j*4)) || fabs(field<float>(n,j*4))>1000000) return false;
    }
    for(size_t i=0;i<edges;i++) {
        auto e=es+i*120;
        if(field<int>(e,0)>=0 || field<unsigned>(e,0x48)>1 || field<unsigned>(e,0x28)>2) return false;
        for(size_t off:{size_t(8),size_t(12)}) {
            int id=field<int>(e,off); bool found=false;
            for(size_t j=0;j<nodes;j++) if(field<int>(ns+j*24,0x14)==id) found=true;
            if(!found) return false;
        }
        for(size_t j=0;j<6;j++) if(!std::isfinite(field<float>(e,0x10+j*4)) || fabs(field<float>(e,0x10+j*4))>1000000) return false;
        for(size_t off:{size_t(0x10),size_t(0x1c)}) {
            double norm=0;
            for(size_t j=0;j<3;j++) norm+=double(field<float>(e,off+j*4))*field<float>(e,off+j*4);
            if(norm<0.0001) return false;
        }
        if(field<void*>(e,0x30)!=field<void*>(e,0x38)) return false;
    }
    return true;
}
Peer* getPeer(const char* name,bool create) {
    for(auto& p:peers) if(!strcmp(p.origin,name)) return &p;
    if(create) for(auto& p:peers) if(!p.origin[0] || !p.seen || GetTickCount64()-p.seen>4000) {
        clear(p); strcpy_s(p.origin,name);
        if(!p.renderer) {
            p.renderer=originalFactory(factory);
            if(!p.renderer) return nullptr;
            memcpy(p.originalPalette,static_cast<char*>(p.renderer)+0x118,sizeof(p.originalPalette));
            field<void**>(p.renderer,0)=previewVtable;
            originalAdd(scene,p.renderer);
        }
        return &p;
    }
    return nullptr;
}
void* convert(void* result,void* toolkit,void* proposal) {
    void* output=originalConvert(result,toolkit,proposal);
    if(!scene || GetCurrentThreadId()!=guiThread) return output;
    std::string request=readRequest();
    unsigned long long reqSession=0,nonce=0;
    char origin[3]{},mode[8]{},extra=0;
    if(sscanf_s(request.c_str(),"%llu %llu %2[a-z] %7[a-z] %c",&reqSession,&nonce,origin,3u,mode,8u,&extra,1u)!=4
        || reqSession!=session) return output;
    // Consume before evaluating, so any nested conversion cannot claim it.
    write("request","");
    bool terrainChanged=expirePeers();
    editingRemote=true;
    bool ok=false;
    if(!strcmp(mode,"clear")) {
        if(auto p=getPeer(origin,false)) { clear(*p); terrainChanged=true; }
        ok=true;
    } else if(!strcmp(mode,"keep")) {
        if(auto p=getPeer(origin,false)) {
            // Expiration cleared the buffers. Reject keep so Lua resends geometry.
            if(p->seen) { p->seen=GetTickCount64(); ok=true; }
        }
    } else if((!strcmp(mode,"draw") || !strcmp(mode,"drawok") || !strcmp(mode,"drawbad"))
        && safeProposal(output) && count(output,0x18,120)>0) {
        if(auto p=getPeer(origin,true)) {
            alignas(16) unsigned char context[0x70]{}, data[0x790]{};
            at<void*(*)(void*,int)>(0x431560)(context,-1);
            at<void*(*)(void*,void*,void*,void*,void*,void*)>(0xa072b0)(data,toolkit,nullptr,output,nullptr,context);
            clear(*p);
            terrainTarget=field<void*>(p->renderer,0x50);
            const float offset[3]{};
            const std::unordered_map<int,std::pair<int,float>> empty;
            // Tint is consumed while generating the buffers, not just at render time.
            applySenderColor(p->renderer,mode);
            applySenderPalette(*p,senderErrorColor);
            at<void(*)(void*,void*,void*,const float*,const void*,bool,bool,bool)>(0x48d8e0)
                (toolkit,p->renderer,data,offset,&empty,false,false,true);
            if(senderErrorColor>=0) errorColor(p->renderer,false);
            senderColorRenderer=nullptr; senderErrorColor=-1;
            at<DtorFn>(0x3e5030)(data);
            at<DtorFn>(0x3e3d30)(context+0x18);
            p->seen=GetTickCount64(); ok=true;
            terrainChanged=true;
            host->log("[previews] 3D origin=%s edges=%zu mode=%s",origin,count(output,0x18,120),mode);
        }
    }
    editingRemote=false;
    if(terrainChanged) composeTerrain();
    write("ack",std::to_string(session)+" "+std::to_string(nonce)+(ok?" ok":" error"));
    return output;
}
}

extern "C" __declspec(dllexport) int Tpf2mpPluginInit(const Tpf2mpHost* h,Tpf2mpPluginInfo* info) {
    if(!h || h->abiMajor!=TPF2MP_ABI_MAJOR || h->size<sizeof(Tpf2mpHost)) return TPF2MP_ERR_ABI;
    if(!h->buildOk()) return TPF2MP_ERR_BUILD;
    host=h; base=h->moduleBase();
    info->name="previews"; info->version="0.1.5-local"; info->summary="Shared 3D previews with sender error colour";
    uploadHeight=at<HeightFn>(0x34cd90); resetHeight=at<ResetHeightFn>(0x34e5a0);
    // Complete build-35924 setter: renderer state +0x1504 controls error tint.
    const uint8_t colorBytes[]={0x48,0x8b,0x81,0xb8,0x01,0x00,0x00,0x88,0x90,0x04,0x15,0x00,0x00,0xc3};
    if(!h->verifyBytes(0x81df00,colorBytes,sizeof(colorBytes))) return TPF2MP_ERR_BUILD;
    setErrorColor=at<ErrorColorFn>(0x81df00);
    struct Hook { uintptr_t rva; const char* bytes; int size; void* callback; void** original; };
    Hook hooks[]={
        {0x859240,"\x48\x89\x4c\x24\x08\x53\x55\x56\x57\x41\x54\x41\x55\x41\x56",15,(void*)makeFactory,(void**)&originalFactory},
        {0x6d32e0,"\x48\x89\x54\x24\x10\x48\x83\xec\x28\x4c\x8b\x81\xc8\x04\x00\x00",16,(void*)addRenderable,(void**)&originalAdd},
        {0x6d22d0,"\x48\x89\x4c\x24\x08\x57\x48\x83\xec\x30\x48\xc7\x44\x24\x20\xfe\xff\xff\xff",19,(void*)destroyScene,(void**)&originalSceneDtor},
        {0x20e72f0,"\x40\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8d\xac\x24\x00\xfc\xff\xff",20,(void*)convert,(void**)&originalConvert},
        {0x817f70,"\x48\x8b\xc4\x55\x57\x41\x54\x41\x56\x41\x57\x48\x8d\x68\xa1",15,(void*)clearRenderer,(void**)&originalClear},
        {0x8191d0,"\x48\x8b\xc4\x55\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8d\x6c\x24\x80",17,(void*)endHeight,(void**)&originalEndHeight},
        {0x814b20,"\x48\x8b\xc4\x55\x57\x41\x56\x48\x8d\x6c\x24\x80\x48\x81\xec\x80\x01\x00\x00",19,(void*)destroyRenderer,(void**)&originalRendererDtor}
    };
    for(const auto& hook:hooks) if(!h->verifyBytes(hook.rva,(const uint8_t*)hook.bytes,hook.size)) {
        h->log("[previews] hook bytes differ at %llx",(unsigned long long)hook.rva); return TPF2MP_ERR_BUILD;
    }
    memcpy(previewVtable,reinterpret_cast<void*>(base+0x30665e0),sizeof(previewVtable));
    previewVtable[1]=(void*)render<1>; previewVtable[2]=(void*)render<2>;
    previewVtable[3]=(void*)render<3>; previewVtable[4]=(void*)render<4>;
    previewVtable[5]=(void*)render<5>; previewVtable[6]=(void*)render<6>;
    for(const auto& hook:hooks) if(!h->installHook(base+hook.rva,hook.callback,hook.size,hook.original)) return TPF2MP_ERR_FAILED;
    return TPF2MP_OK;
}
