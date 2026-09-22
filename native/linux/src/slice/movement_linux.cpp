// Linux SysV counterparts of 3217497/a64e4ea and f6195dd. Only roadentries
// changes an engine-owned order (a road edge's entries, as Windows does).
#include "movement_linux.h"
#include "movement_checks.h"
#include "../paused_checks_linux.h"
#include "../company_ui_checks_linux.h"
#include "../codewrite_linux.h"
#include "train_order_checks.h"
#include "train_order_linux.h"
#include "company_tint_linux.h"
#include "ecs_linux.h"
#include "slice_core.h"
#include "hook.h"
#include "../near_alloc.h"
#include <cstring>
#include "roadspace.h"
#include "trainorder.h"
#include "moveorder.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
uintptr_t movementBase;
std::string movementData;
bool FlagOff(const char* root, const char* data, const char* key)
{
    for (const char* dir : {root, data}) {
        if (!dir || !*dir) continue;
        std::string path = std::string(dir) + "/tpf2_menu_flags.txt";
        FILE* f = fopen(path.c_str(), "r");
        if (!f) continue;
        char line[256]; bool off = false;
        const std::string setting = std::string(key) + "=0";
        while (fgets(line, sizeof(line), f)) if (!strncmp(line, setting.c_str(), setting.size())) off = true;
        fclose(f); return off;
    }
    return false;
}
template<class T, size_t N> bool Check(uintptr_t base, const T (&checks)[N])
{
    for (const auto& c : checks) {
        std::vector<char> bytes(c.size);
        if (!SliceRead(base+c.rva, bytes.data(), bytes.size()) || memcmp(bytes.data(), c.bytes, c.size)) {
            SliceLog("[movement] byte guard failed at %lx; hook stays off\n", (unsigned long)c.rva);
            return false;
        }
    }
    return true;
}

// Keep both engine walks and the filtered std::function invoker intact. The four
// addition sites report their already-rounded term to a per-invocation stack;
// the original sum is still calculated and returned on overflow. No predicate
// is replayed, and nested filtered calls own separate accumulators.
struct FilterContext {
    RoadSpaceAcc acc;
    FilterContext* previous;
    static thread_local FilterContext* active;
    FilterContext() : previous(active) { RoadSpaceBegin(&acc); active=this; }
    ~FilterContext() { active=previous; }
};
thread_local FilterContext* FilterContext::active=nullptr;
void* filteredOriginal;
float FilteredHook(uintptr_t self,uintptr_t id,uintptr_t predicate)
{
    FilterContext context;
    using Fn=float (*)(uintptr_t,uintptr_t,uintptr_t);
    const float original=reinterpret_cast<Fn>(filteredOriginal)(self,id,predicate);
    return RoadSpaceOverflowed(&context.acc) ? original : RoadSpaceResult(&context.acc);
}

void* roadOriginal;
float RoadHook(uintptr_t self,uintptr_t id)
{
    FilterContext context;
    using Fn=float (*)(uintptr_t,uintptr_t);
    const float original=reinterpret_cast<Fn>(roadOriginal)(self,id);
    return RoadSpaceOverflowed(&context.acc) ? original : RoadSpaceResult(&context.acc);
}

struct MoveSample { uint32_t names, ids, rank; bool changed; int64_t named; };
bool Measure(uintptr_t self, uintptr_t world, int n, int type, MoveSample* sample)
{
    if (n < 0 || n > MOVEORDER_MAX_N) return false;
    uintptr_t holder=0; SliceVec records{};
    if (!SliceReadT(self+8,&holder) || !holder ||
        !SliceReadStdVector(holder,MOVEORDER_REC,MOVEORDER_MAX_N,&records) || records.count != size_t(n)) return false;
    try {
        thread_local std::vector<uint8_t> recs;
        thread_local std::vector<int32_t> order;
        thread_local std::vector<TrainOrderKey> keys;
        thread_local std::vector<std::string> names;
        recs.resize(size_t(n)*MOVEORDER_REC); order.resize(n); keys.resize(n); names.resize(n);
        if (n && !SliceRead(records.begin,recs.data(),recs.size())) return false;
        for (int i=0;i<n;++i) {
            const int32_t id=MoveOrderRecId(recs.data(),i);
            char text[TRAINORDER_NAME_MAX+1]; size_t len=0;
            const uintptr_t comp=SliceNameComponent(world,id,type);
            if (comp && SliceReadStdString(comp,text,sizeof(text),&len,TRAINORDER_NAME_MAX)) names[i].assign(text,len);
            keys[i]={names[i].data(),uint32_t(names[i].size()),id,0}; order[i]=i;
        }
        sample->names=MoveOrderNameHash(order.data(),n,keys.data());
        sample->ids=MoveOrderIdHash(order.data(),n,keys.data());
        const auto outcome=MoveOrderRank(order.data(),n,keys.data());
        if (outcome.refused) return false;
        sample->rank=MoveOrderNameHash(order.data(),n,keys.data());
        sample->changed=outcome.changed; sample->named=outcome.named;
        return true;
    } catch (...) { return false; } // private allocations only
}
using UpdateFn = void (*)(uintptr_t,uintptr_t,int,float);
void* moveOriginal[2];
// The Name component's type index, or -1. TypeFind is an engine call, outside
// all allocation catches.
int NameType(uintptr_t world)
{
    const uintptr_t ti=movementBase+0x5a02600;
    using Find=uintptr_t (*)(uintptr_t,const uintptr_t*);
    int type=-1,stored=0;
    const uintptr_t node=reinterpret_cast<Find>(movementBase+0x9e3d50)(world+0x48,&ti);
    if (node && SliceReadT(node+0x10,&stored) && stored>0 && stored<=4097) type=stored-1;
    return type;
}
void Observe(int kind, uintptr_t self, uintptr_t world, int n)
{
    static thread_local bool busy=false;
    if (busy) return;
    const int type=NameType(world);
    busy=true;
    MoveSample sample{};
    const bool ok=Measure(self,world,n,type,&sample);
    busy=false;
    static thread_local std::chrono::steady_clock::time_point last[2];
    const auto now=std::chrono::steady_clock::now();
    if (now-last[kind] < std::chrono::seconds(5)) return;
    last[kind]=now;
    const char* tag=kind ? "airorder" : "shiporder";
    if (!ok) SliceLog("[%s] refused count/read/allocation n=%d; game untouched\n",tag,n);
    else SliceLog("[%s] n=%d named=%lld nameHash=%08x rankHash=%08x idHash=%08x reordered=%d (measurement only)\n",
        tag,n,(long long)sample.named,sample.names,sample.rank,sample.ids,sample.changed);
}
void ShipHook(uintptr_t self,uintptr_t world,int n,float dt)
{ Observe(0,self,world,n); reinterpret_cast<UpdateFn>(moveOriginal[0])(self,world,n,dt); }
void AirHook(uintptr_t self,uintptr_t world,int n,float dt)
{ Observe(1,self,world,n); reinterpret_cast<UpdateFn>(moveOriginal[1])(self,world,n,dt); }

// ROAD ENTRY ORDER (Windows f6195dd). EdgeUseManager's per-edge entries are
// rebuilt on load in save order, while a running world holds arrival order; the
// lead-vehicle search keeps the first entry on an exact tie. After every Add from
// AddToEdgeUseManager the edge's entries are put in name order (the train-order
// rule: ASCII-case-insensitive bytes, then entity id), as Windows does. The edge
// is found through the layout Add's own inlined lookup uses (2e59028..2e590a4):
// indices vector<int> at +30, 72-byte groups at +48..+50 whose vector<EdgeData>
// has 32-byte elements, entries (20 bytes) at EdgeData+8. Anything that does not
// read back as that shape is left exactly as the engine built it.
struct RoadBounds { float back, front; };   // CVec2f: one SSE eightbyte, xmm0
using EdgeAddFn=void (*)(uintptr_t,uint64_t,uint32_t,int32_t,int32_t,RoadBounds);
EdgeAddFn roadEntriesOriginal;
constexpr size_t ROADENTRIES_MAX=512, ROADENTRY_SIZE=20;
std::atomic<uint64_t> reCalls{0}, reSorted{0}, reRefused{0}, reShown{0};
uintptr_t RoadEdgeData(uintptr_t mgr, uint64_t edge)
{
    const int32_t id0=int32_t(uint32_t(edge)), id1=int32_t(uint32_t(edge>>32));
    if (!mgr || id0<0 || id1<0) return 0;
    SliceVec indices{}, groups{}, datas{};
    int32_t group=-1;
    if (!SliceReadStdVector(mgr+0x30,4,1u<<26,&indices) || size_t(id0)>=indices.count ||
        !SliceReadT(indices.begin+size_t(id0)*4,&group) || group<0) return 0;
    if (!SliceReadStdVector(mgr+0x48,72,1u<<24,&groups) || size_t(group)>=groups.count) return 0;
    if (!SliceReadStdVector(groups.begin+size_t(group)*72,32,1u<<24,&datas) || size_t(id1)>=datas.count) return 0;
    return datas.begin+size_t(id1)*32;
}
enum RoadSortResult { RoadUnchanged, RoadSorted, RoadRefused };
RoadSortResult RoadEntriesSortAt(uintptr_t world, uintptr_t mgr, uint64_t edge, int type) noexcept
{
    const uintptr_t ed=RoadEdgeData(mgr,edge);
    if (!ed) return RoadUnchanged;
    SliceVec entries{};
    if (!SliceReadStdVector(ed+8,ROADENTRY_SIZE,1u<<24,&entries)) return RoadRefused;
    const size_t n=entries.count;
    if (n<2) return RoadUnchanged;
    if (n>ROADENTRIES_MAX) return RoadRefused;
    try {
        thread_local std::vector<uint8_t> recs;
        thread_local std::vector<TrainOrderKey> keys;
        thread_local std::vector<std::string> names;
        thread_local std::vector<int32_t> order;
        recs.resize(n*ROADENTRY_SIZE); keys.resize(n); names.resize(n); order.resize(n);
        if (!SliceRead(entries.begin,recs.data(),recs.size())) return RoadRefused;
        for (size_t i=0;i<n;++i) {
            int32_t id=0; memcpy(&id,recs.data()+i*ROADENTRY_SIZE,4);
            char text[TRAINORDER_NAME_MAX+1]; size_t len=0;
            const uintptr_t comp=world ? SliceNameComponent(world,id,type) : 0;
            if (comp && SliceReadStdString(comp,text,sizeof(text),&len,TRAINORDER_NAME_MAX)) names[i].assign(text,len);
            else names[i].clear();
            keys[i]={names[i].data(),uint32_t(names[i].size()),id,0}; order[i]=int32_t(i);
        }
        // insertion sort, as Windows: stable, the same order for the same keys
        for (size_t i=1;i<n;++i) {
            const int32_t t=order[i]; size_t j=i;
            for (; j>0; --j) {
                const int c=TrainOrderNameCmp(keys[t],keys[order[j-1]]);
                if (!(c<0 || (c==0 && keys[t].id<keys[order[j-1]].id))) break;
                order[j]=order[j-1];
            }
            order[j]=t;
        }
        bool changed=false;
        for (size_t i=0;i<n;++i) if (order[i]!=int32_t(i)) { changed=true; break; }
        if (!changed) return RoadUnchanged;
        // The engine's own allocation, just read back whole, inside its Add caller.
        uint8_t* out=reinterpret_cast<uint8_t*>(entries.begin);
        for (size_t i=0;i<n;++i) memcpy(out+i*ROADENTRY_SIZE,recs.data()+size_t(order[i])*ROADENTRY_SIZE,ROADENTRY_SIZE);
        if (++reShown<=4) {
            int32_t first=0; memcpy(&first,out,4);
            SliceLog("[roadentries] edge with %zu vehicles re-ordered by name (first now entity %d)\n",n,first);
        }
        return RoadSorted;
    } catch (...) { return RoadRefused; } // private allocations only
}

bool ReadCompanies(const std::string& data, const char* letter)
{
    if (data.empty()) return false;
    if (letter && *letter) {
        FILE* f=fopen((data+"/lockstep_status_"+letter+".txt").c_str(),"r");
        if (f) {
            char line[512]{};
            const bool yes=fgets(line,sizeof(line),f) && strstr(line," cm=companies");
            fclose(f);
            if (yes) return true;
        }
    }
    FILE* f=fopen((data+"/mp_company_cfg.txt").c_str(),"r");
    bool yes=false;
    if (f) { char line[64]{}; yes=fgets(line,sizeof(line),f) && !strncmp(line,"companies",9); fclose(f); }
    return yes;
}
bool Companies()
{
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static std::chrono::steady_clock::time_point last;
    static bool cached=false;
    const auto now=std::chrono::steady_clock::now();
    if (now-last<std::chrono::seconds(2)) return cached;
    last=now;
    char letter[8]{};
    SliceInstance(letter,sizeof(letter));
    return cached=ReadCompanies(movementData,letter);
}
// Same bounded wire format and permissive missing-file fallback as Windows.
// File IO stays behind a mutex and a two-second cache, off the hover hot path.
struct StationPermissions {
    int count=0, pids[256]{}, cids[256]{};
    char codes[256][96]{};
    void Read(const std::string& data) {
        *this=StationPermissions{};
        if (data.empty()) return;
        FILE* f=fopen((data+"/mp_company_perms.txt").c_str(),"r");
        if (!f) return;
        char line[160];
        while (fgets(line,sizeof(line),f)) {
            int a=0,b=0; char code[96]{};
            if (sscanf(line,"pid %d %d",&a,&b)==2) {
                if (count<256) { pids[count]=a; cids[count++]=b; }
            } else if (sscanf(line,"open %d %95s",&a,code)==2 && a>=1 && a<256) {
                memcpy(codes[a],code,sizeof(code));
            }
        }
        fclose(f);
    }
    bool Allows(int owner,int mine) const {
        int oc=0,mc=0;
        for (int i=0;i<count;++i) {
            if (pids[i]==owner) oc=cids[i];
            if (pids[i]==mine) mc=cids[i];
        }
        if (!oc || !mc || oc==mc || oc<1 || oc>=256 || !codes[oc][0]) return true;
        const char* code=codes[oc];
        if (!strcmp(code,"*")) return true;
        if (!strcmp(code,"-")) return false;
        while (*code) {
            if (atoi(code)==mc) return true;
            while (*code && *code!=',') ++code;
            if (*code) ++code;
        }
        return false;
    }
};
bool StationsPermitted(int owner,int mine)
{
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static StationPermissions cached;
    static std::chrono::steady_clock::time_point last;
    const auto now=std::chrono::steady_clock::now();
    if (last.time_since_epoch().count()==0 || now-last>=std::chrono::seconds(2)) {
        cached.Read(movementData); last=now;
    }
    return cached.Allows(owner,mine);
}
struct UiPatch { uintptr_t rva; const char* before; const char* after; size_t size; };
static const UiPatch iconPatches[] = {
    {0x1383b78, "\x75\x86", "\x90\x90", 2},
    {0x138ba6f, "\x74\x5e", "\xeb\x5e", 2}, // paged owner storage
    {0x138bacd, "\x75\xa2", "\x90\x90", 2}, // contiguous owner storage
    {0x1090cb5, "\x0f\x84\x5d\x01\x00\x00", "\xe9\x5e\x01\x00\x00\x90", 6},
};
static const UiPatch foreignWindowPatches[] = {
    {0x1446d54, "\x0f\x85\xb0\x00\x00\x00", "\x66\x0f\x1f\x44\x00\x00", 6},
};
template<size_t N> bool ApplyUiPatches(uintptr_t base,const UiPatch (&patches)[N])
{
    // Check the whole group before writing, and roll back if a write fails.
    for (const auto& p:patches) {
        char actual[6];
        if (!SliceRead(base+p.rva,actual,p.size) || memcmp(actual,p.before,p.size)) return false;
    }
    for (size_t i=0;i<N;++i) {
        const auto& p=patches[i]; int error=0;
        if (Tpf2mpCodeWriteSelf(base+p.rva,reinterpret_cast<const uint8_t*>(p.after),p.size,&error)!=TPF2MP_CW_OK) {
            for (size_t j=0;j<=i;++j) {
                const auto& undo=patches[j];
                if (Tpf2mpCodeWriteSelf(base+undo.rva,reinterpret_cast<const uint8_t*>(undo.before),undo.size,&error)!=TPF2MP_CW_OK)
                    SliceLog("[company-ui] rollback failed at %lx errno=%d\n",(unsigned long)undo.rva,error);
            }
            return false;
        }
    }
    return true;
}
bool InstallCompanyUi(uintptr_t base,const char* root,const char* data)
{
    bool ok=true;
    // The engine walk first, and unconditionally: slice-lines needs it for the
    // company rename even when the icon tint is switched off.
    SliceEcsSetBase(base);
    if (!SliceEcsAnchored(base))
        SliceLog("[company-ui] the engine's component walk is unavailable on this image: "
                 "the icon tint and the company rename stay off\n");
    if (!FlagOff(root,data,"showicons")) {
        const bool ready=Check(base,kIconChecks) && ApplyUiPatches(base,iconPatches);
        SliceLog("[showicons] %s: four Linux owner branches\n",ready ? "installed" : "OFF");
        ok &= ready;
    }
    if (!FlagOff(root,data,"foreignwindows")) {
        const bool ready=Check(base,kForeignWindowChecks) && ApplyUiPatches(base,foreignWindowPatches);
        SliceLog("[foreignwindows] %s: native command barrier and Lua foreign edit guard retained\n",ready ? "installed" : "OFF");
        ok &= ready;
    }
    // The HUD icon tint is native: the Linux addStyleClass contract and the
    // non-asserting owner walk were settled in the running game (see
    // company_tint_linux.cpp and docs/re/linux/DEV_D6DB920F.md).
    // Not folded into `ok`: the tint is cosmetic, and a refusal here must not
    // take multiplayer startup with it the way a failed permission patch does.
    SliceInstallCompanyTint(base, root, data);
    SliceLog("[company-ui] vehicle icon colour pending native draw implementation\n");
    return ok;
}
bool InstallPausedTick(uintptr_t base, const char* root, const char* data)
{
    if (FlagOff(root,data,"pausedtick")) {
        SliceLog("[pausedtick] OFF (pausedtick=0)\n"); return true;
    }
    if (!Check(base,kPausedChecks)) return false;
    static const uint8_t nop[] = {0x0f,0x1f,0x44,0x00,0x00};
    int error=0;
    const int result=Tpf2mpCodeWriteSelf(base+0xa61860,nop,sizeof(nop),&error);
    SliceLog("[pausedtick] %s: paused GameTime advance at a61860 (write=%d errno=%d)\n",
        result==TPF2MP_CW_OK ? "installed" : "OFF",result,error);
    return result==TPF2MP_CW_OK;
}
}
extern "C" {
__attribute__((visibility("hidden"))) uintptr_t SliceFilteredResumeA=0, SliceFilteredResumeB=0, SliceRoadResumeA=0, SliceRoadResumeB=0;
}
extern "C" __attribute__((visibility("hidden")))
void SliceFilteredTerm(float term) noexcept
{ if (FilterContext::active) RoadSpaceAdd(&FilterContext::active->acc,term); }
extern "C" void SliceRoadRelayA();
extern "C" void SliceRoadRelayB();
extern "C" void SliceFilteredRelayA();
extern "C" void SliceFilteredRelayB();
#include "filtered_relays_linux.h"
extern "C" { __attribute__((visibility("hidden"))) uintptr_t SliceStationResume=0; }
extern "C" __attribute__((visibility("hidden")))
int SliceStationAllow(int owner,int local) { return owner==local || (Companies() && StationsPermitted(owner,local)); }
extern "C" void SliceStationRelay();
// Replaces `call Add` at 16c484a (rdi=manager, rsi=EdgeId, edx=forward,
// ecx=entity, r8d=component, xmm0=bounds; r12 holds AddToEdgeUseManager's
// engine). Add takes no r9, so the relay hands the engine over in it and
// tail-jumps: the C++ side returns straight to AddToEdgeUseManager.
extern "C" __attribute__((visibility("hidden")))
void SliceRoadEntriesAdd(uintptr_t mgr,uint64_t edge,uint32_t forward,int32_t entity,int32_t comp,uintptr_t world,RoadBounds bounds)
{
    roadEntriesOriginal(mgr,edge,forward,entity,comp,bounds);
    ++reCalls;
    if (!world || !mgr) return;
    const RoadSortResult r=RoadEntriesSortAt(world,mgr,edge,NameType(world));
    if (r==RoadSorted) ++reSorted;
    else if (r==RoadRefused) ++reRefused;
}
extern "C" void SliceRoadEntriesRelay();
asm(".text\n.hidden SliceRoadEntriesRelay\n.type SliceRoadEntriesRelay,@function\n"
    "SliceRoadEntriesRelay:\nendbr64\nmov %r12,%r9\njmp SliceRoadEntriesAdd\n"
    ".size SliceRoadEntriesRelay,.-SliceRoadEntriesRelay\n");
// Entered via JMP with the engine's stack already aligned for CALL. The next
// instruction after the stolen cmp/sete/movzx is the engine's epilogue jump.
asm(".text\n.hidden SliceStationRelay\n.type SliceStationRelay,@function\n"
    "SliceStationRelay:\nmov %eax,%edi\nmov 0x28(%rbx),%esi\n"
    "call SliceStationAllow\njmp *SliceStationResume(%rip)\n"
    ".size SliceStationRelay,.-SliceStationRelay\n");

bool SliceInstallMovement(uintptr_t base,const char* root,const char* data)
{
    movementBase=base; movementData=data ? data : "";
    bool ok=InstallPausedTick(base,root,data);
    ok &= InstallCompanyUi(base,root,data);
    if (!FlagOff(root,data,"roadspace")) {
        void* unusedPlainA=nullptr; void* unusedPlainB=nullptr;
        SliceRoadResumeA=base+0x2e558eb; SliceRoadResumeB=base+0x2e55988;
        const bool ready=Check(base,kRoadChecks) &&
            InstallHook(base+0x2e558e4,reinterpret_cast<void*>(&SliceRoadRelayA),7,&unusedPlainA) &&
            InstallHook(base+0x2e55981,reinterpret_cast<void*>(&SliceRoadRelayB),7,&unusedPlainB) &&
            InstallHook(base+0x2e557d0,reinterpret_cast<void*>(&RoadHook),7,&roadOriginal);
        SliceLog("[roadspace] plain overload %s\n",ready ? "installed" : "OFF");
        ok &= ready;
        void* unusedA=nullptr; void* unusedB=nullptr;
        SliceFilteredResumeA=base+0x2e55dbf;
        SliceFilteredResumeB=base+0x2e5601e;
        const bool filtered=Check(base,kFilteredChecks) &&
            InstallHook(base+0x2e55db5,reinterpret_cast<void*>(&SliceFilteredRelayA),10,&unusedA) &&
            InstallHook(base+0x2e56014,reinterpret_cast<void*>(&SliceFilteredRelayB),10,&unusedB) &&
            InstallHook(base+0x2e55b30,reinterpret_cast<void*>(&FilteredHook),8,&filteredOriginal);
        SliceLog("[roadspace] filtered overload %s; original predicate walk retained\n",filtered ? "installed" : "OFF");
        ok &= filtered;
    }
    // Independent of trainorder=0; only Name layout/accessor checks are shared.
    bool namesOk=true;
    for (const auto& c:kTrainOrderChecks) {
        if (c.rva>=0x1750000 || c.rva==0xc0cf10) continue;
        uint8_t bytes[128];
        namesOk &= SliceRead(base+c.rva,bytes,c.size) && !memcmp(bytes,c.bytes,c.size);
    }
    uintptr_t name=0; char text[sizeof("N3ecs9component4NameE")];
    namesOk &= SliceReadT(base+0x5a02608,&name) && SliceRead(name,text,sizeof(text)) && !memcmp(text,"N3ecs9component4NameE",sizeof(text));
    if (FlagOff(root,data,"roadentries")) {
        SliceLog("[roadentries] OFF (roadentries=0 in tpf2_menu_flags.txt) -- a road edge's vehicle entries keep the engine's arrival/load order\n");
    } else {
        roadEntriesOriginal=reinterpret_cast<EdgeAddFn>(base+0x2e58f70);
        const bool ready=namesOk && Check(base,kRoadEntriesChecks) &&
            Tpf2mpRedirectCall(base+0x16c484a,base+0x2e58f70,reinterpret_cast<void*>(&SliceRoadEntriesRelay));
        SliceLog("[roadentries] %s: a road edge's vehicle entries are kept in name order (AddToEdgeUseManager call at 16c484a)\n",
            ready ? "installed" : "OFF");
        ok &= ready;
    }
    if (!FlagOff(root,data,"shiporder")) {
        const bool ready=namesOk && Check(base,kShipChecks) && InstallHook(base+0x16d75c0,reinterpret_cast<void*>(&ShipHook),8,&moveOriginal[0]);
        SliceLog("[shiporder] %s (measurement only)\n",ready ? "installed" : "OFF"); ok &= ready;
    }
    if (!FlagOff(root,data,"airorder")) {
        const bool ready=namesOk && Check(base,kAirChecks) && InstallHook(base+0x16692e0,reinterpret_cast<void*>(&AirHook),8,&moveOriginal[1]);
        SliceLog("[airorder] %s (measurement only)\n",ready ? "installed" : "OFF"); ok &= ready;
    }
    if (!FlagOff(root,data,"sharedstations")) {
        void* unused=nullptr; SliceStationResume=base+0x10c03c9;
        const bool ready=Check(base,kStationsChecks) && InstallHook(base+0x10c03c0,reinterpret_cast<void*>(&SliceStationRelay),9,&unused);
        SliceLog("[sharedstations] %s (companies mode only)\n",ready ? "installed" : "OFF"); ok &= ready;
    }
    return ok;
}
