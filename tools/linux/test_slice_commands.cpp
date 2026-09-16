// Off-game integration harness: real libstdc++ objects + guarded process reads,
// with the slice-core registration, Add, session and inject services modelled.
// Run: tools/linux/test_slice_commands.sh
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <sys/uio.h>
#include <unistd.h>
#include "../../native/linux/src/slice/slice_vehicles.cpp"
#include "../../native/linux/src/slice/slice_lines.cpp"
#include "../../native/linux/src/slice/slice_time.cpp"

static bool live = true, cancelAvailable = true, writeFails = false;
static uint64_t nowMs = 100;
static std::vector<SliceFactoryHandler> handlers;
static std::vector<SliceAddObserverFn> observers;
static std::vector<SliceHookSpec> hooks;
static std::vector<std::string> writes;
static bool armed;
static SliceArm arm;
static uintptr_t armedCmd;
static std::vector<uint8_t> payload(0xd50);
static uintptr_t command[7];
static std::string testDataDir;
static void* overrideDone;
static void (*overrideAfter)(void*,void*);
static void* overrideContext;
static bool stepInstalled, directInstalled;

uintptr_t SliceImageBase() { return 0x100000000; }
const char* SliceDataDir() { return testDataDir.c_str(); }
bool SliceSessionLive() { return live; }
bool SliceCancelAvailable() { return cancelAvailable; }
bool SliceInstance(char* out, size_t cap) { if (cap < 2) return false; std::strcpy(out, "A"); return true; }
uint64_t SliceNowMs() { return nowMs; }
void SliceLog(const char*, ...) {}
void SliceTerrainPollHeldTools() {}
const char* SliceOutcomeName(SliceOutcome) { return "test"; }
bool SliceRead(uintptr_t src, void* dst, size_t n)
{
    if (!n) return true;
    iovec local{dst,n}, remote{reinterpret_cast<void*>(src),n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(n);
}
bool SliceReadable(uintptr_t src, size_t n)
{
    std::vector<uint8_t> bytes(n);
    return SliceRead(src, bytes.data(), n);
}
bool SliceReadStdVector(uintptr_t src, size_t stride, size_t max, SliceVec* out)
{
    uintptr_t v[3];
    if (!SliceRead(src, v, sizeof(v)) || !stride) return false;
    if (!v[0]) { *out = {}; return !v[1] && !v[2]; }
    if (v[0] > v[1] || v[1] > v[2] || (v[1]-v[0]) % stride || (v[2]-v[0]) % stride ||
        (v[1]-v[0])/stride > max || !SliceReadable(v[0], v[1]-v[0])) return false;
    *out = {v[0], (v[1]-v[0])/stride};
    return true;
}
bool SliceReadStdString(uintptr_t src, char* out, size_t cap, size_t* len, size_t max)
{
    uintptr_t h[4];
    if (!SliceRead(src, h, sizeof(h)) || h[1] > max || h[1] >= cap ||
        (h[0] == src+16 ? h[1] > 15 : !h[0] || h[2] < h[1]) ||
        !SliceRead(h[0], out, h[1]+1) || out[h[1]]) return false;
    *len = h[1]; return true;
}
bool SliceStdFunctionParts(uintptr_t p, uintptr_t* manager, uintptr_t* invoker)
{ return SliceReadT(p+16, manager) && SliceReadT(p+24, invoker); }
int SliceCommandTag(uintptr_t p)
{
    uintptr_t data; uint8_t tag;
    return SliceReadT(p, &data) && SliceReadT(data+0xd48, &tag) ? tag : -1;
}
void SliceRecordAppend(SliceRecord* r, const char* s, size_t n)
{
    if (r->failed) return;
    char* p = static_cast<char*>(std::realloc(r->data, r->len+n+1));
    if (!p) { r->failed = true; return; }
    r->data = p; r->cap = r->len+n+1;
    std::memcpy(p+r->len,s,n); r->len+=n; p[r->len]=0;
}
void SliceRecordPrintf(SliceRecord* r, const char* fmt, ...)
{
    va_list ap; va_start(ap,fmt); char* p = nullptr; int n=vasprintf(&p,fmt,ap); va_end(ap);
    if (n<0) r->failed=true; else SliceRecordAppend(r,p,size_t(n));
    std::free(p);
}
void SliceRecordFree(SliceRecord* r) { std::free(r->data); *r={}; }
SliceInjectResult SliceInjectWrite(const SliceRecord& r, SliceArmedLine a)
{
    if (writeFails || r.failed) return SliceInjectResult::NotWritten;
    writes.push_back(std::string(a==SliceArmedLine::One ? "ARMED 1\n" : a==SliceArmedLine::Zero ? "ARMED 0\n" : "")+
        std::string(r.data ? r.data : "",r.len));
    return SliceInjectResult::Written;
}
bool SliceOnFactory(const SliceFactoryHandler& h) { handlers.push_back(h); return true; }
bool SliceOnAdd(const char*, SliceAddObserverFn f, void*) { observers.push_back(f); return true; }
bool SliceRegisterHook(const SliceHookSpec& s) { hooks.push_back(s); return true; }
bool SliceHookInstalled(uintptr_t rva) { return rva==slice_time::kStep ? stepInstalled : rva==0xa2d650 && directInstalled; }
bool SliceOverrideAddDone(const SliceAddCall&, void* done, void (*after)(void*,void*), void* ctx)
{ overrideDone=done;overrideAfter=after;overrideContext=ctx;return true; }
bool SliceArmCancel(const SliceFactoryCall& c, const SliceArm& a)
{
    if (!c.armable || c.script || !cancelAvailable) return false;
    if (armed && arm.landed) arm.landed(nullptr,SliceOutcome::Superseded,arm.ctx);
    arm=a; armed=true; armedCmd=c.rdi; return true;
}
void SliceDisarm(const SliceFactoryCall& c) { if (c.rdi==armedCmd) armed=false; }
bool SliceShipAndArm(const SliceFactoryCall& c,const SliceArm& a,const SliceRecord& r)
{
    if (live && SliceArmCancel(c,a)) {
        if (SliceInjectWrite(r,SliceArmedLine::One)!=SliceInjectResult::NotWritten) return true;
        SliceDisarm(c); return false;
    }
    return false;
}

static SliceFactoryCall Call(uintptr_t rva, uintptr_t ret = 1)
{
    if (armed && arm.landed) arm.landed(nullptr,SliceOutcome::Superseded,arm.ctx);
    armed=false; writes.clear();
    static SliceFactoryInfo info;
    info={}; info.rva=rva;
    switch(rva) {
        case slice_vehicles::kBuy: info.tag=13; break;
        case slice_lines::kUpdate: info.tag=5; break;
        case slice_lines::kCreate: info.tag=3; break;
        case slice_time::kDate: info.tag=26; break;
        case slice_time::kCalendar: info.tag=1; break;
        default: break;
    }
    payload.assign(0xd50,0); payload[0xd48]=info.tag;
    const int32_t result=-1; std::memcpy(payload.data()+0x38,&result,4);
    command[0]=uintptr_t(payload.data());
    SliceFactoryCall c{}; c.factory=&info; c.rdi=uintptr_t(command); c.retRva=ret; c.armable=true;
    return c;
}
static void Capture(const SliceFactoryCall& c)
{ for (const auto& h:handlers) if(h.factoryRva==c.factory->rva && h.onEntry) h.onEntry(c,nullptr); }
static bool Add(void* done=nullptr,uintptr_t site=0)
{
    uintptr_t ret=1;
    SliceAddCall add{&ret,nullptr,command,done,nullptr,0,site,0};
    overrideDone=nullptr;overrideAfter=nullptr;overrideContext=nullptr;
    for (auto f:observers) f(add,nullptr);
    if (overrideDone) {
        auto* fn=static_cast<uintptr_t*>(overrideDone);
        assert(fn[2]==SliceAddr(0x10d44b0) && fn[3]==SliceAddr(0x10d6c30));
        fn[2]=0; // Original Add transfers ownership into its own callback queue.
        overrideAfter(overrideDone,overrideContext);
    }
    if (!armed) return false;
    armed=false;
    if (arm.prepareCancel && !arm.prepareCancel(add,arm.ctx)) {
        if(arm.landed) arm.landed(&add,live ? SliceOutcome::Blocked : SliceOutcome::RanNatively,arm.ctx);
        return false;
    }
    if(arm.landed) arm.landed(&add,SliceOutcome::CancelledNotFired,arm.ctx);
    return true;
}

struct VehiclePart {
    int32_t model; bool reversed; uint8_t padding[3]; std::vector<int> loads;
    float color[3]; uint32_t padding2; std::string logo;
    int64_t purchase; float maintenance,target; std::vector<bool> automatic;
};
static_assert(sizeof(VehiclePart)==0x88);
struct Config { std::vector<VehiclePart> parts; std::vector<int> groups; };
struct Stop {
    int32_t group,station,terminal,pad;
    std::vector<std::pair<int,int>> alternatives;
    int32_t loadMode; float min,max; uint32_t pad2;
    std::vector<uint64_t> waypoints; std::vector<bool> load,unload; std::vector<int> cfg;
};
static_assert(sizeof(Stop)==0xb8);
struct Line { std::vector<Stop> stops; float wait; uint32_t padding; int info[3]; };

static void TestVehicles()
{
    Config cfg{}; cfg.parts.resize(1); auto& p=cfg.parts[0];
    p.model=17; p.loads={3,5}; p.color[0]=.1f; p.color[1]=.2f; p.color[2]=.3f; p.automatic={true,false}; cfg.groups={0};
    auto c=Call(slice_vehicles::kBuy); c.rdx=4;c.rcx=81;c.r8=uintptr_t(&cfg);
    Capture(c); assert(armed && writes.empty());
    alignas(8) uint8_t lambda[0x58]{}; int32_t line=23; std::memcpy(lambda+0x30,&line,4);
    uintptr_t fn[4]={uintptr_t(lambda),0,SliceAddr(0x126eb30),SliceAddr(0x1272350)};
    assert(Add(fn)); assert(writes.size()==1);
    assert(writes[0]=="ARMED 1\nVBUY 81 1 17 2 3 5 0.1000 0.2000 0.3000 1 1 1 0\nVBUYLINE 23\n");
    // Actual vector<bool> word boundary, signed low half, and erased tail bits.
    p.automatic.assign(65,true); p.loads.assign(65,0);
    SliceRecord rec{}; assert(slice_vehicles::AutoLoad(&rec,uintptr_t(&p.automatic)));
    assert(std::string(rec.data)==" 3 -1 -1 1"); SliceRecordFree(&rec);
    // An iterator with a nonzero start offset is normalized before serialization.
    uint64_t source[2]={0x8000000000000000ULL,1};
    uintptr_t bits[5]={uintptr_t(source),63,uintptr_t(source+1),1,uintptr_t(source+2)};
    assert(slice_vehicles::AutoLoad(&rec,uintptr_t(bits))); assert(std::string(rec.data)==" 1 3"); SliceRecordFree(&rec);
    bits[1]=64; assert(!slice_vehicles::AutoLoad(&rec,uintptr_t(bits))); SliceRecordFree(&rec);
    c=Call(slice_vehicles::kBuy);c.r8=1;Capture(c);assert(!armed && writes.empty());
    c=Call(slice_vehicles::kReplace);c.r8=1;c.rcx=uintptr_t(&cfg);Capture(c);assert(!armed && writes.empty());
    c=Call(slice_vehicles::kBuy);c.rcx=81;c.r8=uintptr_t(&cfg);Capture(c);writeFails=true;
    assert(!Add(fn));assert(writes.empty());writeFails=false;
    c=Call(slice_vehicles::kReverse);c.rdx=44;c.script=true;Capture(c);assert(writes.empty());
    c.script=false;live=false;Capture(c);assert(!armed && writes.empty());live=true;
    std::vector<int> sell{1,5,99}; c=Call(slice_vehicles::kSell);c.rdx=uintptr_t(&sell);Capture(c);
    assert(armed && writes[0]=="ARMED 1\nVSELL 3 1 5 99\n");Add();
    c=Call(slice_vehicles::kBuy);c.rcx=81;c.r8=uintptr_t(&cfg);Capture(c);
    uintptr_t unknown[4]={0,0,1,2};assert(!Add(unknown));assert(writes.empty());
}

static void TestLines()
{
    Line line{};line.wait=180.4f;
    auto c=Call(slice_lines::kUpdate);c.rdx=42;c.rcx=uintptr_t(&line);Capture(c);
    assert(armed && writes[0]=="ARMED 1\nLUPDATE 42 180.399994 0\n");Add();
    line.stops.reserve(2);line.stops.resize(1);auto& s=line.stops[0];s.group=98;s.station=1;s.terminal=2;s.loadMode=3;s.min=20.4f;s.max=10.7f;
    s.alternatives={{2,3},{4,5}};c=Call(slice_lines::kUpdate);c.rdx=42;c.rcx=uintptr_t(&line);Capture(c);
    assert(writes[0]=="ARMED 1\nLUPDATE 42 180.399994 1 98 1 2 3 20.3999996 10.6999998 2 2 3 4 5\n");Add();
    // Preserve waypoint order and 1-based stop positions in both wire records.
    s.waypoints={uint64_t(123) | (uint64_t(2)<<32),uint64_t(456)};
    SliceRecord rec{};
    assert(slice_lines::Decode(&rec,42,uintptr_t(&line)));
    assert(std::string(rec.data).find(" wp=1:123:2,1:456:0")!=std::string::npos);
    SliceRecordFree(&rec);
    std::string waypointName="Route"; const float rgb[]={1,0,0};
    assert(slice_lines::DecodeCreate(&rec,uintptr_t(&waypointName),rgb,uintptr_t(&line)));
    assert(std::string(rec.data).find(" wp=1:123:2,1:456:0 name=Route\n")!=std::string::npos);
    SliceRecordFree(&rec);
    line.stops.push_back(s);
    assert(slice_lines::Decode(&rec,42,uintptr_t(&line)));
    assert(std::string(rec.data).find(",2:123:2,2:456:0")!=std::string::npos);
    SliceRecordFree(&rec);
    line.stops.pop_back();
    for (uint64_t invalid : {uint64_t(0),uint64_t(123)|(uint64_t(0xffffffff)<<32)}) {
        line.stops[0].waypoints={invalid};
        assert(!slice_lines::Decode(&rec,42,uintptr_t(&line))); SliceRecordFree(&rec);
    }
    line.stops[0].waypoints.assign(65,123);
    assert(slice_lines::Decode(&rec,42,uintptr_t(&line))); SliceRecordFree(&rec);
    line.stops[0].waypoints.clear();
    s.min=NAN;c=Call(slice_lines::kUpdate);c.rdx=42;c.rcx=uintptr_t(&line);Capture(c);
    assert(!armed && writes.empty());s.min=0;
    // More than all former caps, large indices, negative/infinite/fractional waits.
    Line large = line;
    large.wait = INFINITY;
    large.stops[0].min = -1.25f; large.stops[0].max = -INFINITY;
    large.stops[0].station = 100; large.stops[0].terminal = 200;
    large.stops[0].alternatives.assign(40, {100, 200});
    large.stops[0].waypoints.assign(80, uint64_t(123) | (uint64_t(9999)<<32));
    large.stops.resize(70, large.stops[0]);
    assert(slice_lines::Decode(&rec,42,uintptr_t(&large)));
    assert(std::string(rec.data).find("inf 70 98 100 200 3 -1.25 -inf 40") != std::string::npos);
    assert(std::string(rec.data).find("70:123:9999") != std::string::npos);
    SliceRecordFree(&rec);

    c=Call(slice_lines::kUpdate,0x132c513);c.rdx=42;c.rcx=uintptr_t(&line);Capture(c);
    assert(armed && arm.done==SliceDone::Required && writes.empty());
    int counter=1;uintptr_t lambda=uintptr_t(&counter);
    uintptr_t fn[4]={uintptr_t(&lambda),0,SliceAddr(0x1323eb0),SliceAddr(0x13228e0)};
    assert(Add(fn));assert(writes[0].rfind("ARMED 1\nLUPDATE",0)==0);
    c=Call(slice_lines::kUpdate,0x132c513);c.rdx=42;c.rcx=uintptr_t(&line);Capture(c);
    counter=1;writeFails=true;assert(!Add(fn));writeFails=false;
    assert(counter==0 && writes.empty());
    c=Call(slice_lines::kUpdate,0x132c513);c.rdx=42;c.rcx=uintptr_t(&line);Capture(c);fn[3]=0;
    assert(!Add(fn));assert(writes.empty());
    std::string name="Coal 50%=\xc3\xa9";
    c=Call(slice_lines::kName);c.rdx=7;c.rcx=uintptr_t(&name);Capture(c);
    assert(!armed && writes.empty());
    c=Call(slice_lines::kColor);c.rdx=7;c.xmm[0]=_mm_setr_ps(.25f,.5f,99,99);c.xmm[1]=_mm_setr_ps(.75f,99,99,99);Capture(c);
    assert(!armed && writes.empty());
    c=Call(slice_lines::kCreate);Capture(c);assert(!armed && writes.empty());
}

struct CreatePayload {
    Line line; std::string name; float color[3]; int32_t player,result;
    uint8_t rest[0xd50-0x68];
};
static_assert(offsetof(CreatePayload,name)==0x30 && offsetof(CreatePayload,color)==0x50);
static bool directUsed;
static void FakeDirectSink(void*,void*,void* done)
{
    auto* fn=static_cast<uintptr_t*>(done);
    directUsed=fn[2]==SliceAddr(0x10d44b0) && fn[3]==SliceAddr(0x10d6c30);
    if (directUsed) fn[2]=0; // Test owns no game functor; suppress its nonexistent destructor.
}
static void WriteClaim(uint64_t nonce,const char* raw=nullptr)
{
    FILE* f=std::fopen((testDataDir+"lockstep_lclaim_A.txt").c_str(),"w");assert(f);
    if(raw) std::fprintf(f,"%s",raw);else std::fprintf(f,"%llu",static_cast<unsigned long long>(nonce));
    std::fclose(f);
}
static void TestStrictCreates()
{
    CreatePayload p{};p.line.wait=180;p.name="Identical Line";p.color[0]=127.f/255;p.color[1]=.25f;p.color[2]=1;p.player=4;p.result=-1;
    reinterpret_cast<uint8_t*>(&p)[0xd48]=3;
    slice_lines::g_directSinkTrampoline=reinterpret_cast<void*>(FakeDirectSink);
    directInstalled=true;
    auto create=[&](bool script) {
        auto c=Call(slice_lines::kCreate,script ? 0x1951b68 : 0x2eb1c49);
        c.rsi=uintptr_t(&p.name);c.rcx=uintptr_t(&p.line);c.rdx=4;c.script=script;
        c.xmm[0]=_mm_setr_ps(p.color[0],p.color[1],0,0);c.xmm[1]=_mm_setr_ps(p.color[2],0,0,0);
        Capture(c);return c;
    };
    uintptr_t original[4]={0x1234,0,SliceAddr(0x10d44b0),SliceAddr(0x10d6c30)};
    WriteClaim(100); // A claim already present at capture cannot select it.
    create(false);assert(armed && writes.empty());assert(Add(original));assert(original[2]==0);
    assert(writes[0]=="ARMED 1\nLCREATEX 0.498039216 0.25 1 180 0 name=Identical%20Line\n");
    const uint64_t first=slice_lines::g_creates[0]->id;
    original[2]=SliceAddr(0x10d44b0);create(false);assert(Add(original));
    const uint64_t second=slice_lines::g_creates[1]->id;assert(first!=second);
    create(true);assert(!slice_lines::t_carrier.id);
    // Existing Windows claims select the oldest matching held callback once.
    WriteClaim(101);create(true);assert(!armed && writes.empty());
    assert(slice_lines::t_carrier.id==first);command[0]=uintptr_t(&p);
    uintptr_t luaFn[4]={0,0,777,888};assert(!Add(luaFn,0xa2f5c2));assert(luaFn[2]==777);
    assert(!slice_lines::g_creates[0] && slice_lines::g_creates[1]);
    create(true);assert(!slice_lines::t_carrier.id); // repeated nonce cannot claim the second
    WriteClaim(102,"102 junk");create(true);assert(!slice_lines::t_carrier.id);
    WriteClaim(102);
    timespec oldTimes[2]{};assert(clock_gettime(CLOCK_REALTIME,&oldTimes[0])==0);
    oldTimes[0].tv_sec-=6;oldTimes[1]=oldTimes[0];
    assert(utimensat(AT_FDCWD,(testDataDir+"lockstep_lclaim_A.txt").c_str(),oldTimes,0)==0);
    create(true);assert(!slice_lines::t_carrier.id);
    WriteClaim(103);p.name="Different Line";create(true);assert(!slice_lines::t_carrier.id);p.name="Identical Line";
    create(true);assert(!slice_lines::t_carrier.id); // mismatched factory consumed its nonce
    WriteClaim(104);create(true);assert(slice_lines::t_carrier.id==second);
    p.name="Changed after maker";command[0]=uintptr_t(&p);directUsed=false;
    slice_lines::DirectSink(nullptr,command,luaFn);assert(!directUsed && slice_lines::g_creates[1]);
    p.name="Identical Line";
    WriteClaim(105);create(true);command[0]=uintptr_t(&p);directUsed=false;
    slice_lines::DirectSink(nullptr,command,luaFn);assert(directUsed && !slice_lines::g_creates[1]);
    // I/O failure restores the source function's ownership; core blocks the create.
    original[2]=SliceAddr(0x10d44b0);create(false);writeFails=true;assert(!Add(original));writeFails=false;
    assert(original[2]==SliceAddr(0x10d44b0));assert(!slice_lines::g_creates[0]);
    // A mismatched callback emits no replay or legacy read-back event.
    create(false);assert(!Add(luaFn));assert(writes.empty());
    // Actual libstdc++ function move/destruction: no clone and no double release.
    auto state=std::make_shared<int>(0);
    std::function<void(int)> fn=[state](int v){*state=v;};assert(state.use_count()==2);
    auto* held=static_cast<slice_lines::HeldCreate*>(std::calloc(1,sizeof(slice_lines::HeldCreate)));
    static_assert(sizeof(fn)==32);std::memcpy(held->fn,&fn,32);
    const uintptr_t empty=0;std::memcpy(reinterpret_cast<char*>(&fn)+16,&empty,8);
    int arg=9;reinterpret_cast<void (*)(void*,int&&)>(held->fn[3])(held->fn,std::move(arg));assert(*state==9);
    slice_lines::ReleaseCreate(nullptr,held);assert(state.use_count()==1 && !fn);
}

alignas(8) static uint8_t clockObject[0x500];
static int stepMode=0, nesting=0;
static void (*foreignThrow)() = nullptr;
static uintptr_t* ClockFunction()
{
    static uintptr_t fn[4];fn[0]=uintptr_t(clockObject);fn[1]=0;fn[2]=SliceAddr(0xf6ae90);fn[3]=SliceAddr(0xf6af90);return fn;
}
static int& Count() { return *reinterpret_cast<int*>(clockObject+0x478); }
__attribute__((noinline)) static void FakeStep(void* clock,int64_t a,int64_t b)
{
    assert(slice_time::InStep());
    if(stepMode==1) throw std::runtime_error("game exception");
    if(stepMode==4) foreignThrow();
    if(stepMode==2 && nesting++==0) { slice_time::DoStep(clock,a,b); assert(slice_time::InStep()); }
    if(stepMode==3) {
        auto c=Call(slice_time::kSpeed,slice_time::kButtons);c.rsi=2;Capture(c);
        assert(armed);assert(Add(ClockFunction()));++Count();assert(writes.empty());
    }
}
static void TestTime()
{
    auto c=Call(slice_time::kCalendar,slice_time::kSlider);c.rsi=0;Capture(c);assert(armed && writes.empty());
    assert(Add());assert(writes[0]=="CALSPEED 0\n");
    c=Call(slice_time::kSpeed,slice_time::kToggle);c.rsi=4;Capture(c);assert(Add());
    assert(writes[0]=="SPEEDBTN 4 toggle\n");
    c=Call(slice_time::kSpeed,slice_time::kToggle);c.rsi=65;Capture(c);assert(!armed && writes.empty());
    c=Call(slice_time::kSpeed,slice_time::kToggle);c.rsi=4;Capture(c);writeFails=true;assert(!Add());writeFails=false;
    c=Call(slice_time::kSpeed,slice_time::kButtons);c.rsi=4;Capture(c);assert(!armed); // no installed DoStep
    slice_time::g_stepTrampoline=reinterpret_cast<void*>(FakeStep);
    // A retained trampoline from a failed patch must not enable cancellations.
    c=Call(slice_time::kSpeed,slice_time::kButtons);c.rsi=4;Capture(c);assert(!armed);
    stepInstalled=true;
    c=Call(slice_time::kSpeed,slice_time::kButtons);c.rsi=4;Capture(c);assert(Add(ClockFunction()));
    assert(writes[0]=="SPEEDBTN 4 button\n");assert(Count()==0);++Count();
    slice_time::DoStep(clockObject,1,2);assert(Count()==0);assert(!slice_time::InStep());
    // A failed append is blocked just like a cancelled Add: the UI still
    // increments its count after Add, so the reserved compensation must survive.
    c=Call(slice_time::kSpeed,slice_time::kButtons);c.rsi=4;Capture(c);writeFails=true;
    assert(!Add(ClockFunction()));writeFails=false;assert(writes.empty());++Count();
    slice_time::DoStep(clockObject,1,2);assert(Count()==0);
    // A callback of unexpected type must leave Add and the counter untouched.
    c=Call(slice_time::kSpeed,slice_time::kButtons);c.rsi=4;Capture(c);assert(!Add());assert(writes.empty());
    stepMode=3;slice_time::DoStep(clockObject,1,2);assert(Count()==1 && writes.empty());
    stepMode=0;nowMs+=999;slice_time::DoStep(clockObject,1,2);assert(Count()==1);
    ++nowMs;slice_time::DoStep(clockObject,1,2);assert(Count()==0);
    // Reused Clock storage with different button pointers must not inherit a decrement.
    c=Call(slice_time::kSpeed,slice_time::kButtons);c.rsi=2;Capture(c);assert(Add(ClockFunction()));++Count();
    clockObject[0x4b0]=1;slice_time::DoStep(clockObject,1,2);assert(Count()==1);Count()=0;
    // Nested scope restoration and stale-scope invalidation after foreign-style unwind.
    stepMode=2;nesting=0;slice_time::DoStep(clockObject,1,2);assert(!slice_time::InStep());
    stepMode=1;try {slice_time::DoStep(clockObject,1,2);assert(false);} catch(const std::runtime_error&) {}
    assert(!slice_time::InStep());
    stepMode=0;c=Call(slice_time::kSpeed,slice_time::kButtons);c.rsi=3;Capture(c);assert(Add(ClockFunction()));
    assert(writes[0]=="SPEEDBTN 3 button\n");++Count();slice_time::DoStep(clockObject,1,2);assert(Count()==0);
}

int main(int argc, char** argv)
{
    testDataDir=argc>2 ? argv[2] : "/tmp/";
    slice_vehicles_area_SliceRegister();slice_lines_area_SliceRegister();slice_time_area_SliceRegister();
    assert(handlers.size()==14 && hooks.size()==2 && hooks[1].steal==14 && hooks[1].expectedLen==27);
    TestVehicles();TestLines();TestStrictCreates();TestTime();
    if (argc > 1) {
        void* foreign = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(foreign);
        foreignThrow = reinterpret_cast<void (*)()>(dlsym(foreign,"SliceTestForeignThrow"));
        auto foreignCatch = reinterpret_cast<bool (*)(void (*)())>(dlsym(foreign,"SliceTestForeignCatch"));
        assert(foreignThrow && foreignCatch); stepMode=4;
        assert(foreignCatch(+[] { slice_time::DoStep(clockObject,1,2); }));
        assert(!slice_time::InStep());
        dlclose(foreign);
    }
    std::puts("slice vehicles, lines and time: decoder, cancel, write failure, clock and unwind checks passed");
}
