// Linux build-35924 cosmetic BuilderRenderers. See docs/re/linux/PREVIEW_PLUGIN.md.
// Addresses and layouts below were derived from the Linux ELF, never rebased from PE.
// The GUI constructs a command solely to use scripting::Convert. It never sends it.
// This plugin reads the converted proposal, evaluates it and uploads preview buffers;
// it has no command-dispatch or applyProposal entry point.
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <pthread.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include "plugin/tpf2mp_plugin.h"
#include "preview_game_guard.h"

namespace {
uint64_t nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return uint64_t(ts.tv_sec)*1000+uint64_t(ts.tv_nsec)/1000000;
}
pid_t threadId() { return static_cast<pid_t>(syscall(SYS_gettid)); }
// The game owns every input object. Probe through the kernel rather than
// dereferencing arbitrary converted proposal/renderer pointers in this library.
bool readMemory(const void* from,void* to,size_t n) {
    uintptr_t address=reinterpret_cast<uintptr_t>(from);
    if(!n) return true;
    if(address<0x10000 || address>UINTPTR_MAX-n) return false;
    iovec local{to,n}, remote{const_cast<void*>(from),n};
    ssize_t got;
    do { got=process_vm_readv(getpid(),&local,1,&remote,1,0); } while(got<0 && errno==EINTR);
    return got==static_cast<ssize_t>(n);
}
bool readable(const void* p,size_t n) {
    if(n>0x100000) return false;
    unsigned char scratch[4096];
    for(size_t off=0;off<n;off+=sizeof(scratch))
        if(!readMemory(static_cast<const char*>(p)+off,scratch,std::min(sizeof(scratch),n-off))) return false;
    return true;
}
pthread_mutex_t previewMutex=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
struct Lock {
    Lock() { pthread_mutex_lock(&previewMutex); }
    ~Lock() { pthread_mutex_unlock(&previewMutex); }
};
const Tpf2mpHost* host;
uintptr_t base;
void* scene;
pid_t guiThread;
uint64_t session;
alignas(16) unsigned char factory[0xb8];
bool factoryLive, hooksReady, renderThreadVerified;
using EmptyEntityMap=std::unordered_map<int,std::pair<int,float>>;
static_assert(sizeof(EmptyEntityMap)==0x38,"audited Linux libstdc++ empty map size");
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
using RenderFn = void (*)(void*,void*,void*);
using UpdateFn = void (*)(void*,void*,float);
UpdateFn rendererUpdate;
RenderFn originalPrimaryRender;
using CreateDataFn=void*(*)(void*,void*,void*,void*,void*,void*);
using UploadProposalFn=void(*)(void*,void*,void*,const float*,const void*,bool,bool,bool);
CreateDataFn createData;
UploadProposalFn uploadProposal;
DtorFn destroyData,destroyContext,deleteRenderer;
AddFn removeRenderable;
using CopyFactoryFn=void(*)(void*,const void*);
CopyFactoryFn copyFactory;
// Height uploads are global to the UI terrain, unlike the model renderers.
// Local descriptors remain owned by their BuilderRenderer until Clear/destroy.
std::vector<void*> localHeightRenderers;
bool editingRemote, disposing, renderThreadLogged;
bool threadMismatch, nativeOperationFailed, serviceFailed;
template<class Fn,class... Args> bool nativeCall(Fn fn,Args... args) {
    const bool ok=callGame(fn,args...);
    if(!ok) nativeOperationFailed=true;
    return ok;
}
template<class R,class Fn,class... Args> bool nativeResult(R* result,Fn fn,Args... args) {
    const bool ok=callGameResult(result,fn,args...);
    if(!ok) nativeOperationFailed=true;
    return ok;
}
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
    nativeCall(setErrorColor,renderer,invalid);
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
    auto palette=static_cast<unsigned char*>(p.renderer)+0xf8;
    memcpy(palette,p.originalPalette,sizeof(p.originalPalette));
    // Four matching RGBA pairs: per-segment error selection must use the
    // sender's palette as well as the renderer-wide/terrain error flag.
    if(invalid==0) memcpy(palette+0x40,p.originalPalette,0x40);
    else if(invalid==1) memcpy(palette,p.originalPalette+0x40,0x40);
}

bool active(const Peer& p) { return p.seen && nowMs()-p.seen<=4000; }
bool isPeerRenderer(void* r) {
    for(const auto& p:peers) if(p.renderer==r) return true;
    return false;
}
bool hasRemoteTerrain() {
    for(const auto& p:peers) if(p.renderer && p.seen) return true;
    return false;
}
void uploadRendererHeight(void* r) {
    if(!r || !readable(r,0x1b0) || field<void*>(r,0x50)!=terrainTarget || !field<bool>(r,0xd0)) return;
    auto state=field<unsigned char*>(r,0x198);
    if(!state || !readable(state+0x1830,24)) return;
    if(field<void*>(state,0x1830)!=field<void*>(state,0x1838))
        nativeCall(uploadHeight,terrainTarget,state+0x1830,field<bool>(r,0xd4));
}
void composeTerrain() {
    if(!terrainTarget || disposing) return;
    nativeCall(resetHeight,terrainTarget,true);
    // Remote uploads first; one's own tool keeps priority where areas overlap.
    for(const auto& p:peers) if(active(p)) uploadRendererHeight(p.renderer);
    for(void* r:localHeightRenderers) uploadRendererHeight(r);
}
void forgetLocalHeight(void* r) {
    localHeightRenderers.erase(std::remove(localHeightRenderers.begin(),localHeightRenderers.end(),r),localHeightRenderers.end());
}
// Forwarding frames carry no active C++ cleanup across the original game call.
// Our helpers own locks/allocations and catch only our runtime's exceptions;
// calls they initiate into the game use the dynamic-runtime guard.
void afterClear(void* r) noexcept {
    try {
        Lock lock;
        if(!hooksReady) return;
        forgetLocalHeight(r);
        if(!editingRemote && !disposing && scene && threadId()==guiThread && hasRemoteTerrain()) composeTerrain();
    } catch(...) {}
}
void clearRenderer(void* r,bool models,bool terrain) {
    originalClear(r,models,terrain);
    afterClear(r);
}
bool endRemoteHeight(void* r) noexcept {
    try {
        Lock lock;
        if(!hooksReady || !isPeerRenderer(r)) return false;
        const bool enabled=field<bool>(r,0xd0);
        if(r==senderColorRenderer && senderErrorColor>=0) errorColor(r,false);
        field<bool>(r,0xd0)=false;
        nativeCall(originalEndHeight,r);
        field<bool>(r,0xd0)=enabled;
        return true;
    } catch(...) { return true; }
}
void afterEndHeight(void* r) noexcept {
    try {
        Lock lock;
        if(hooksReady && scene && field<bool>(r,0xd0) && threadId()==guiThread &&
           std::find(localHeightRenderers.begin(),localHeightRenderers.end(),r)==localHeightRenderers.end())
            localHeightRenderers.push_back(r);
    } catch(...) {}
}
void endHeight(void* r) {
    if(endRemoteHeight(r)) return;
    originalEndHeight(r);
    afterEndHeight(r);
}
void beforeDestroyRenderer(void* r) noexcept {
    try {
        Lock lock;
        if(!hooksReady) return;
        forgetLocalHeight(r);
        for(auto& peer:peers) if(peer.renderer==r) peer=Peer{};
        if(senderColorRenderer==r) { senderColorRenderer=nullptr; senderErrorColor=-1; }
        if(!disposing && !editingRemote && scene && threadId()==guiThread && hasRemoteTerrain()) composeTerrain();
    } catch(...) {}
}
void destroyRenderer(void* r) {
    beforeDestroyRenderer(r);
    originalRendererDtor(r);
}

std::string path(const char* name) { return std::string(host->dataDir())+"tpf2mp_preview_native_"+name+".txt"; }
bool write(const char* name, const std::string& value) {
    FILE* f=fopen(path(name).c_str(),"wb");
    if(!f) return false;
    bool ok=fwrite(value.data(),1,value.size(),f)==value.size() && fputs("\nend\n",f)>=0;
    return fclose(f)==0 && ok;
}
void resetMailbox() noexcept {
    try { write("ready",""); write("request",""); write("ack",""); } catch(...) {}
}
std::string readRequest() {
    FILE* f=fopen(path("request").c_str(),"rb");
    if(!f) return {};
    char buf[192]{}; size_t n=fread(buf,1,sizeof(buf),f); fclose(f);
    if(n==sizeof(buf) || memchr(buf,0,n)) return {};
    // Mixed-platform test files may use CRLF. Only normalize actual CRLF pairs.
    size_t k=0;
    for(size_t i=0;i<n;++i) {
        if(buf[i]=='\r' && i+1<n && buf[i+1]=='\n') continue;
        buf[k++]=buf[i];
    }
    if(k<5 || memcmp(buf+k-5,"\nend\n",5)) return {};
    return std::string(buf,k-5);
}
void clear(Peer& p) {
    p.seen=0;
    if(p.renderer) nativeCall(originalClear,p.renderer,true,false);
}
bool expirePeers() {
    bool changed=false;
    for(auto& p:peers) if(p.seen && !active(p)) { clear(p); changed=true; }
    return changed;
}
// Itanium slot 2 is Update(this, component, float seconds): dt arrives in XMM0.
// The ordinary render passes in slots 3..7 take three GP-register arguments.
void updateRenderer(void* self,void* component,float dt) noexcept {
    try {
        Lock lock;
        if(!hooksReady || !renderThreadVerified || threadMismatch || serviceFailed || threadId()!=guiThread) return;
        for(const auto& p:peers) if(p.renderer==self) {
            if(active(p)) nativeCall(rendererUpdate,self,component,dt);
            return;
        }
    } catch(...) {}
}
template<int Slot> void render(void* self,void* a,void* b) {
    try {
    Lock lock;
    if(!hooksReady || !renderThreadVerified || threadMismatch || serviceFailed) return;
    if(threadId()!=guiThread) {
        if(!threadMismatch) host->log("[previews] render/GUI thread mismatch; native previews disabled for this scene");
        threadMismatch=true;
        write("ready","");
        return;
    }
    if constexpr(Slot==3) {
        if(!renderThreadLogged) {
            host->log("[previews] render/GUI thread match=%d",threadId()==guiThread);
            renderThreadLogged=true;
        }
        if(threadId()==guiThread && expirePeers()) composeTerrain();
    }
    for(const auto& p:peers) if(p.renderer==self) {
        if(p.seen && nowMs()-p.seen<=4000)
            nativeCall(reinterpret_cast<RenderFn*>(base+0x59bba48)[Slot],self,a,b);
        return;
    }
    } catch(...) {}
}
void observeRenderThread(void* target) noexcept {
    try {
        Lock lock;
        if(!hooksReady || target!=scene || !scene || threadMismatch || serviceFailed) return;
        if(threadId()!=guiThread) {
            threadMismatch=true;
            write("ready","");
            host->log("[previews] render/GUI thread mismatch; keeping Lua previews");
        } else if(!renderThreadVerified) {
            if(!write("ready",std::to_string(session))) return;
            renderThreadVerified=true;
            host->log("[previews] GUI/render thread verified; independent renderer service ready");
        }
    } catch(...) {}
}
void primaryRender(void* self,void* target,void* helper) {
    originalPrimaryRender(self,target,helper);
    observeRenderThread(target);
}
// Itanium ABI: preserve offset-to-top and RTTI, then BOTH destructor entries.
// Update and five render passes occupy slots 2..7; Windows has one destructor slot.
void* previewVtableStorage[10];
void** previewVtable=previewVtableStorage+2;

void dispose() {
    disposing=true;
    if(terrainTarget) nativeCall(resetHeight,terrainTarget,true);
    localHeightRenderers.clear();
    for(auto& p:peers) {
        if(p.renderer) {
            if(scene) nativeCall(removeRenderable,scene,p.renderer);
            clear(p);
            nativeCall(deleteRenderer,p.renderer);
        }
        p=Peer{};
    }
    scene=nullptr;
    terrainTarget=nullptr; disposing=false; renderThreadLogged=false; threadMismatch=false; renderThreadVerified=false; nativeOperationFailed=false; serviceFailed=false;
    if(factoryLive) {
        auto manager=field<void(*)(void*,const void*,int)>(factory,0x90);
        if(manager) nativeCall(manager,factory+0x80,factory+0x80,3);
        memset(factory,0,sizeof(factory));
        factoryLive=false;
    }
    write("ready","");
    host->log("[previews] scene disposed");
}
void afterFactory(void* source,uintptr_t caller) noexcept {
    try {
        Lock lock;
        // First StreetBuilder during CGameUI construction retains a complete
        // native factory. Its std::function owns a cloned native callable.
        if(hooksReady && !factoryLive && caller-base==0xe7d14a) {
            if(!readMemory(source,factory,sizeof(factory))) return;
            memset(factory+0x80,0,32);
            if(nativeCall(copyFactory,factory+0x80,static_cast<char*>(source)+0x80)) factoryLive=true;
            else memset(factory,0,sizeof(factory));
        }
    } catch(...) {}
}
void* makeFactory(void* source) {
    void* result=originalFactory(source);
    afterFactory(source,reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    return result;
}
void afterAdd(void* target,uintptr_t caller) noexcept {
    try {
        Lock lock;
        if(hooksReady && factoryLive && !scene && caller-base==0xffb246) {
            uintptr_t vtable=0;
            if(!readMemory(target,&vtable,sizeof(vtable)) || vtable!=base+0x5a199c8) return;
            scene=target; guiThread=threadId();
            session=(uint64_t(getpid())<<32)^nowMs();
            renderThreadVerified=false;
            write("ready","");
            host->log("[previews] scene retained; awaiting native render thread observation");
        }
    } catch(...) {}
}
void addRenderable(void* target,void* object) {
    originalAdd(target,object);
    afterAdd(target,reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}
void beforeDestroyScene(void* target) noexcept {
    try { Lock lock; if(hooksReady && target==scene) dispose(); } catch(...) {}
}
void destroyScene(void* target) {
    beforeDestroyScene(target);
    originalSceneDtor(target);
}
size_t count(void* p,size_t offset,size_t stride) {
    uintptr_t v[3]{};
    if(!stride || !readMemory(static_cast<char*>(p)+offset,v,sizeof(v))) return SIZE_MAX;
    auto begin=v[0],end=v[1],capacity=v[2];
    if(end<begin || capacity<end || (end-begin)%stride || (capacity-begin)%stride ||
       (!begin && (end || capacity)) || !readable(reinterpret_cast<void*>(begin),end-begin)) return SIZE_MAX;
    return (end-begin)/stride;
}
bool safeProposal(void* p) {
    if(!readable(p,0x3c0)) return false;
    const size_t nodes=count(p,0,24), edges=count(p,0x18,120);
    const size_t constructions=count(p,0x2a0,0x8f0);
    if(constructions>1 || nodes>(constructions?384:48) || edges>(constructions?192:24) || (edges && !nodes)) return false;
    // Accept one independently converted construction, never a replacement or
    // demolition. Its generated streets remain temporary, disconnected pieces.
    for(size_t off:{size_t(0x30),size_t(0x48),size_t(0xd0),size_t(0xe8),size_t(0x288)})
        if(count(p,off,off==0x30 ? 24 : off==0x48 ? 120 : off==0xe8 ? 256 : 4)!=0) return false;
    if(constructions) {
        auto c=field<unsigned char*>(p,0x2a0);
        uintptr_t stringData=0;size_t stringLen=0;
        memcpy(&stringData,c,8);memcpy(&stringLen,c+8,8);
        char filename[181]{};
        if(!stringLen || stringLen>180 || !readMemory(reinterpret_cast<void*>(stringData),filename,stringLen+1) ||
           filename[stringLen]!=0) return false;
        const std::string_view file(filename,stringLen);
        if(file.empty() || file.size()>180 || file.front()=='/' || file.find("..")!=std::string_view::npos
            || file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-")!=std::string_view::npos
            || file.size()<4 || file.substr(file.size()-4)!=".con") return false;
        const float* t=reinterpret_cast<float*>(c+0x738);
        for(size_t i=0;i<16;i++) if(!std::isfinite(t[i]) || fabs(t[i])>((i>=12&&i<=14)?1000000:100)) return false;
        if(fabs(t[3])+fabs(t[7])+fabs(t[11])+fabs(t[15]-1)>0.001) return false;
        double det=t[0]*(double(t[5])*t[10]-double(t[6])*t[9])-t[4]*(double(t[1])*t[10]-double(t[2])*t[9])
            +t[8]*(double(t[1])*t[6]-double(t[2])*t[5]);
        if(fabs(det)<0.000001 || fabs(det)>1000) return false;
    }
    auto ns=field<unsigned char*>(p,0), es=field<unsigned char*>(p,0x18);
    for(size_t i=0;i<nodes;i++) {
        auto n=ns+i*24;
        if(field<int>(n,0x14)>=0) return false;
        for(size_t j=0;j<i;++j) if(field<int>(ns+j*24,0x14)==field<int>(n,0x14)) return false;
        for(size_t j=0;j<3;j++) if(!std::isfinite(field<float>(n,j*4)) || fabs(field<float>(n,j*4))>1000000) return false;
    }
    for(size_t i=0;i<edges;i++) {
        auto e=es+i*120;
        if(field<int>(e,0)>=0 || field<unsigned>(e,0x48)>1 || field<unsigned>(e,0x28)>2 ||
           field<int>(e,8)==field<int>(e,12)) return false;
        for(size_t j=0;j<i;++j) if(field<int>(es+j*120,0)==field<int>(e,0)) return false;
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
        if(count(e,0x30,8)!=0) return false;
    }
    return true;
}
Peer* getPeer(const char* name,bool create) {
    Peer* selected=nullptr;
    for(auto& p:peers) if(!strcmp(p.origin,name)) { selected=&p; break; }
    if(!create) return selected;
    if(!selected) for(auto& p:peers) if(!p.origin[0] || !p.seen || nowMs()-p.seen>4000) {
        clear(p); snprintf(p.origin,sizeof(p.origin),"%s",name); selected=&p; break;
    }
    if(!selected) return nullptr;
    auto& p=*selected;
    if(!p.renderer) {
        if(!nativeResult(&p.renderer,originalFactory,factory) || !p.renderer) return nullptr;
        memcpy(p.originalPalette,static_cast<char*>(p.renderer)+0xf8,sizeof(p.originalPalette));
        field<void**>(p.renderer,0)=previewVtable;
        if(!nativeCall(originalAdd,scene,p.renderer)) {
            nativeCall(deleteRenderer,p.renderer); p.renderer=nullptr; return nullptr;
        }
    }
    return &p;
}
// Exact default Context from the Linux Lua maker, 0x197129a..0x1971302.
// The empty libstdc++ unordered_map uses its own single bucket at +0x48.
void initContext(unsigned char* context) {
    memset(context,0,0x68);
    context[1]=1; context[9]=1;
    field<float>(context,4)=1.0f;
    field<float>(context,0x10)=0.75f;
    field<int32_t>(context,0x14)=-1;
    field<void*>(context,0x18)=context+0x48;
    field<uint64_t>(context,0x20)=1;
    field<float>(context,0x38)=1.0f;
}
void afterConvert(void* output,void* toolkit,uintptr_t caller) noexcept {
    try {
    Lock lock;
    if(!hooksReady || !scene || !renderThreadVerified || threadMismatch || serviceFailed || threadId()!=guiThread ||
       (base && caller-base!=0x197128d)) return;
    std::string request=readRequest();
    unsigned long long reqSession=0,nonce=0;
    char origin[3]{},mode[8]{},extra=0;
    if(sscanf(request.c_str(),"%llu %llu %2[a-z] %7[a-z] %c",&reqSession,&nonce,origin,mode,&extra)!=4
        || reqSession!=session) return;
    // Consume before evaluating, so any nested conversion cannot claim it.
    if(!write("request","")) return;
    nativeOperationFailed=false;
    bool terrainChanged=expirePeers();
    editingRemote=true;
    bool ok=false;
    if(!strcmp(mode,"clear")) {
        if(auto p=getPeer(origin,false)) { clear(*p); terrainChanged=true; }
        ok=true;
    } else if(!strcmp(mode,"keep")) {
        if(auto p=getPeer(origin,false)) {
            // Expiration cleared the buffers. Reject keep so Lua resends geometry.
            if(p->seen) { p->seen=nowMs(); ok=true; }
        }
    } else if((!strcmp(mode,"draw") || !strcmp(mode,"drawok") || !strcmp(mode,"drawbad"))
        && safeProposal(output) && (count(output,0x18,120)>0 || count(output,0x2a0,0x8f0)>0)) {
        if(auto p=getPeer(origin,true)) {
            alignas(16) unsigned char context[0x68]{}, data[0x900]{};
            initContext(context);
            if(!nativeCall(createData,data,toolkit,nullptr,output,nullptr,context)) {
                nativeCall(destroyContext,context);
                editingRemote=false;
                clear(*p);
                serviceFailed=true;
                write("ready","");
                write("ack",std::to_string(session)+" "+std::to_string(nonce)+" error");
                return;
            }
            clear(*p);
            terrainTarget=field<void*>(p->renderer,0x50);
            const float offset[3]{};
            const EmptyEntityMap empty;
            // Tint is consumed while generating the buffers, not just at render time.
            applySenderColor(p->renderer,mode);
            applySenderPalette(*p,senderErrorColor);
            const bool uploaded=nativeCall(uploadProposal,toolkit,p->renderer,data,offset,&empty,false,false,true);
            if(senderErrorColor>=0) errorColor(p->renderer,false);
            senderColorRenderer=nullptr; senderErrorColor=-1;
            nativeCall(destroyData,data);
            nativeCall(destroyContext,context);
            if(uploaded && !nativeOperationFailed) { p->seen=nowMs(); ok=true; }
            else clear(*p);
            terrainChanged=true;
            host->log("[previews] 3D origin=%s edges=%zu constructions=%zu mode=%s",origin,count(output,0x18,120),count(output,0x2a0,0x8f0),mode);
        }
    }
    editingRemote=false;
    if(terrainChanged) composeTerrain();
    if(nativeOperationFailed) {
        ok=false; serviceFailed=true;
        for(auto& p:peers) clear(p);
        write("ready","");
        host->log("[previews] native preview operation threw; keeping Lua previews for this scene");
    }
    write("ack",std::to_string(session)+" "+std::to_string(nonce)+(ok?" ok":" error"));
    } catch(...) {
        editingRemote=false; senderColorRenderer=nullptr; senderErrorColor=-1;
    }
}
void* convert(void* result,void* toolkit,void* proposal) {
    void* output=originalConvert(result,toolkit,proposal);
    afterConvert(output,toolkit,reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    return output;
}
}

extern "C" __attribute__((visibility("default"))) int Tpf2mpPluginInit(const Tpf2mpHost* h,Tpf2mpPluginInfo* info) {
    if(!h || !info || h->abiMajor!=TPF2MP_ABI_MAJOR || h->size<sizeof(Tpf2mpHost) ||
       !h->buildOk || !h->moduleBase || !h->log || !h->verifyBytes || !h->installHook || !h->dataDir)
        return TPF2MP_ERR_ABI;
    if(!h->buildOk() || !h->moduleBase()) return TPF2MP_ERR_BUILD;
    host=h; base=h->moduleBase();
    resetMailbox();
    if(!h->cfgBool || !h->cfgBool("previews","enabled",0)) return TPF2MP_ERR_DISABLED;
    if(!resolveGameRuntime()) {
        h->log("[previews] game C++ exception runtime unavailable"); return TPF2MP_ERR_FAILED;
    }
    hooksReady=false;
    createData=at<CreateDataFn>(0x162f050); uploadProposal=at<UploadProposalFn>(0xef03c0);
    destroyData=at<DtorFn>(0xde3df0); destroyContext=at<DtorFn>(0xdd8820);
    deleteRenderer=at<DtorFn>(0x1394d00); removeRenderable=at<AddFn>(0x11da5a0);
    copyFactory=at<CopyFactoryFn>(0x13fa5b0);
    info->name="previews"; info->version="0.1.6-linux"; info->summary="Shared road, rail and experimental construction previews";
    uploadHeight=at<HeightFn>(0xd0a070); resetHeight=at<ResetHeightFn>(0xd09950);
    const uint8_t colorBytes[]={0xf3,0x0f,0x1e,0xfa,0x48,0x8b,0x87,0x98,0x01,0x00,0x00,
                               0x40,0x88,0xb0,0x84,0x16,0x00,0x00,0xc3};
    if(!h->verifyBytes(0x1391f00,colorBytes,sizeof(colorBytes))) return TPF2MP_ERR_BUILD;
    setErrorColor=at<ErrorColorFn>(0x1391f00);
    struct Hook { uintptr_t rva; const char* bytes; int size; void* callback; void** original; };
    Hook hooks[]={
        {0x1395340,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x57\x49\x89\xf7\x41\x56",15,(void*)primaryRender,(void**)&originalPrimaryRender},
        {0x13fa4b0,"\xf3\x0f\x1e\xfa\x55\x48\x8d\xb7\x80\x00\x00\x00\x48\x89\xe5",15,(void*)makeFactory,(void**)&originalFactory},
        {0x11da630,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x55\x41\x54\x4c\x8d\x6d\xd8",16,(void*)addRenderable,(void**)&originalAdd},
        {0x11d99e0,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x55\x41\x54\x53\x48\x89\xfb",16,(void*)destroyScene,(void**)&originalSceneDtor},
        {0x2e237b0,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x57\x49\x89\xff\x41\x56",15,(void*)convert,(void**)&originalConvert},
        {0x13981d0,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55",14,(void*)clearRenderer,(void**)&originalClear},
        {0x1397170,"\xf3\x0f\x1e\xfa\x55\x48\x89\xf8\x48\x89\xe5\x41\x57\x41\x56",15,(void*)endHeight,(void**)&originalEndHeight},
        {0x13948d0,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x55\x41\x54\x49\x89\xfc",15,(void*)destroyRenderer,(void**)&originalRendererDtor}
    };
    for(const auto& hook:hooks) if(!h->verifyBytes(hook.rva,(const uint8_t*)hook.bytes,hook.size)) {
        h->log("[previews] hook bytes differ at %llx",(unsigned long long)hook.rva); return TPF2MP_ERR_BUILD;
    }
    struct Guard { uintptr_t rva; const char* bytes; unsigned len; };
    const Guard guards[]={
        {0x13fa5b0,"\xf3\x0f\x1e\xfa\x48\xc7\x47\x10\x00\x00\x00\x00\x48\x8b\x46\x10",16},
        {0xd0a070,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x57\x41\x56\x49\x89\xf6",15},
        {0xd09950,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x89\xf6",15},
        {0x162f050,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x57\x49\x89\xff\x41\x56",15},
        {0xde3df0,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55",14},
        {0xdd8820,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x55\x41\x54\x53\x48\x89\xfb",16},
        {0xef03c0,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x57\x49\x89\xd7\x41\x56",15},
        {0x11da5a0,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x53\x48\x89\xfb\x48\x8d\x55\xe8",16},
        {0x1394d00,"\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x53\x48\x89\xfb\x48\x83\xec\x08",16}
    };
    for(const auto& guard:guards) if(!h->verifyBytes(guard.rva,(const uint8_t*)guard.bytes,guard.len)) {
        h->log("[previews] dependency bytes differ at %lx",(unsigned long)guard.rva); return TPF2MP_ERR_BUILD;
    }
    const uintptr_t table[]={0,base+0x5a20748,base+0x13948d0,base+0x1394d00,base+0x138eba0,
                              base+0x1395340,base+0xe160b0,base+0x1395910,base+0x139c320,base+0x1395c90};
    if(!h->verifyBytes(0x59bba38,reinterpret_cast<const uint8_t*>(table),sizeof(table))) return TPF2MP_ERR_BUILD;
    memcpy(previewVtableStorage,table,sizeof(table));
    rendererUpdate=at<UpdateFn>(0x138eba0);
    previewVtable[2]=(void*)updateRenderer;
    previewVtable[3]=(void*)render<3>; previewVtable[4]=(void*)render<4>;
    previewVtable[5]=(void*)render<5>; previewVtable[6]=(void*)render<6>; previewVtable[7]=(void*)render<7>;
    for(const auto& hook:hooks) if(!h->installHook(base+hook.rva,hook.callback,hook.size,hook.original)) return TPF2MP_ERR_FAILED;
    hooksReady=true;
    return TPF2MP_OK;
}
