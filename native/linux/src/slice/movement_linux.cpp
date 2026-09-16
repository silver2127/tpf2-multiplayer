// Linux SysV counterparts of 3217497/a64e4ea. No engine-owned order is changed.
#include "movement_linux.h"
#include "movement_checks.h"
#include "../paused_checks_linux.h"
#include "../codewrite_linux.h"
#include "train_order_checks.h"
#include "train_order_linux.h"
#include "slice_core.h"
#include "hook.h"
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
void Observe(int kind, uintptr_t self, uintptr_t world, int n)
{
    static thread_local bool busy=false;
    if (busy) return;
    // TypeFind is an engine call, outside all allocation catches.
    const uintptr_t ti=movementBase+0x5a02600;
    using Find=uintptr_t (*)(uintptr_t,const uintptr_t*);
    int type=-1,stored=0;
    const uintptr_t node=reinterpret_cast<Find>(movementBase+0x9e3d50)(world+0x48,&ti);
    if (node && SliceReadT(node+0x10,&stored) && stored>0 && stored<=4097) type=stored-1;
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
int SliceStationAllow(int owner,int local) { return owner==local || Companies(); }
extern "C" void SliceStationRelay();
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
