// Included by menu_game_linux.cpp: share its verified frame gate and the
// game's exception runtime. No private C++ cleanup encloses an engine call.
#include "native_io_linux.h"
#include <deque>
#include <sys/syscall.h>

namespace NativeIo {
namespace {
enum class State { Idle, QueuedSave, QueuedLoad, QueuedPause, Saving, Loading, Pausing };
struct Control {
    std::mutex mutex;
    std::deque<Event> events;
    State state=State::Idle;
    std::string operation,name;
    uintptr_t world=0,requestedWorld=0;
    unsigned uiThread=0,commandThread=0;
    bool enabled=false,accepted=false;
};
Control& C(){static auto* value=new Control;return *value;}
unsigned Tid(){return unsigned(syscall(SYS_gettid));}
template<class T>T& Field(uintptr_t p,size_t off){return *reinterpret_cast<T*>(p+off);}
void EmitLocked(const char* step,bool ok,const char* detail="") {
    C().events.push_back({C().operation,step,detail,ok});
}
void Finish(const char* step,bool ok,const char* detail="") {
    std::lock_guard<std::mutex> lock(C().mutex);
    C().state=State::Idle; EmitLocked(step,ok,detail);
}
using SaveFactory=void*(*)(void*,void*,void*,void*,void*,bool,bool);
static void* saveFactoryOriginal=nullptr;
static void* saveDoneOriginal=nullptr;
static void* legacyEventOriginal=nullptr;
static std::atomic<bool> actionsHeld{false};
static int LegacyEventHook(void* binding,void* lua) {
    // This legacy Lua binding returns the number of results (normally zero).
    // Stop before it creates a Command or a completion; the guide script
    // otherwise produces saveevent every GUI frame even while paused.
    if(actionsHeld.load())return 0;
    return reinterpret_cast<int(*)(void*,void*)>(legacyEventOriginal)(binding,lua);
}
struct SaveCall { uintptr_t world; GStr name; bool captured; };
static thread_local SaveCall* ownSave=nullptr;
static void* SaveFactoryHook(void* out,void* context,void* metadata,void* name,void* picture,bool automatic,bool flag) {
    if(ownSave) {name=&ownSave->name;automatic=false;ownSave->captured=true;}
    return reinterpret_cast<SaveFactory>(saveFactoryOriginal)(out,context,metadata,name,picture,automatic,flag);
}
static void SaveDoneHook(void* closure,const bool* success,const void* error) {
    const uintptr_t world=*static_cast<uintptr_t*>(closure);
    const bool ok=success&&*success;
    reinterpret_cast<void(*)(void*,const bool*,const void*)>(saveDoneOriginal)(closure,success,error);
    std::lock_guard<std::mutex> lock(C().mutex);
    if(C().state==State::Saving&&C().requestedWorld==world) {
        C().commandThread=Tid(); C().state=State::Idle;
        EmitLocked("saved",ok,ok?"":"Engine save failed");
    }
}
static void SaveBody(void* raw) {
    auto* call=static_cast<SaveCall*>(raw);
    ownSave=call;
    reinterpret_cast<void(*)(void*)>(g_base+0xfeb560)(reinterpret_cast<void*>(call->world));
    ownSave=nullptr;
}
struct Ticket { std::atomic<unsigned> refs{1}; uintptr_t world; std::string id; unsigned pass=0; };
struct Function { void* data[2]{}; void* manager=nullptr; void* invoke=nullptr; };
static_assert(sizeof(Function)==32,"libstdc++ function ABI");
static bool Manager(void* dst,const void* src,int op) {
    auto* ticket=*static_cast<Ticket* const*>(src);
    if(op==1) *static_cast<const void**>(dst)=src;
    else if(op==2){++ticket->refs;*static_cast<Ticket**>(dst)=ticket;}
    else if(op==3 && --ticket->refs==0)delete ticket;
    else if(op==0)*static_cast<void**>(dst)=nullptr;
    return false;
}
struct PauseCall { uintptr_t world; Function callback; alignas(16) unsigned char command[0x38]{}; uintptr_t connection=0,progress[2]{}; bool live=false; };
static void PauseBody(void* raw) {
    auto& call=*static_cast<PauseCall*>(raw);
    reinterpret_cast<void*(*)(void*,int)>(g_base+0x15eb600)(call.command,0);
    call.live=true;
    const auto game=Field<uintptr_t>(call.world,0x448),queue=Field<uintptr_t>(game,0x158);
    reinterpret_cast<void*(*)(void*,void*,void*,void*,void*)>(g_base+0x15da840)(
        &call.connection,reinterpret_cast<void*>(queue),call.command,&call.callback,call.progress);
    reinterpret_cast<void(*)(void*)>(g_base+0x3190430)(&call.connection);
    call.connection=0; call.live=false;
    reinterpret_cast<void(*)(void*)>(g_base+0x15d8f30)(call.command);
}
static void PauseCleanup(void* raw) {
    auto& call=*static_cast<PauseCall*>(raw);
    if(call.connection){reinterpret_cast<void(*)(void*)>(g_base+0x3190430)(&call.connection);call.connection=0;}
    if(call.live){call.live=false;reinterpret_cast<void(*)(void*)>(g_base+0x15d8f30)(call.command);}
}
static void PauseNow(uintptr_t world,const std::string& id,unsigned pass);
static void PauseDone(const void* storage,const unsigned char* command) {
    auto* ticket=*static_cast<Ticket* const*>(storage);
    bool again=false;
    {
        std::lock_guard<std::mutex> lock(C().mutex);
        if(C().state!=State::Pausing||C().operation!=ticket->id||C().world!=ticket->world)return;
        C().commandThread=Tid();
        const auto game=Field<uintptr_t>(ticket->world,0x448),queue=Field<uintptr_t>(game,0x158);
        const auto data=Field<uintptr_t>(queue,0);
        const bool drained=Field<uintptr_t>(data,0)==Field<uintptr_t>(data,8);
        const bool ok=command&&command[0x30];
        again=ok&&!drained&&ticket->pass<64;
        if(!again){C().state=State::Idle;EmitLocked("paused",ok&&drained,
            !ok?"Engine pause failed":!drained?"Command producers did not become idle":"");}
    }
    if(again)PauseNow(ticket->world,ticket->id,ticket->pass+1);
}
static void PauseNow(uintptr_t world,const std::string& id,unsigned pass) {
    auto* ticket=new Ticket; ticket->world=world;ticket->id=id;ticket->pass=pass;
    PauseCall call{};call.world=world;call.callback.data[0]=ticket;
    call.callback.manager=reinterpret_cast<void*>(&Manager);call.callback.invoke=reinterpret_cast<void*>(&PauseDone);
    const bool threw=tpf2mp_mg_guarded(&PauseBody,&call)!=0;
    if(threw)tpf2mp_mg_guarded(&PauseCleanup,&call);
    if(call.callback.manager)Manager(&call.callback,&call.callback,3);
    if(threw)Finish("paused",false,"Engine pause threw");
}
static bool ValidName(const std::string& s) {
    return s.size()>=4&&s.size()<=15&&s.compare(0,3,"mp_")==0&&
        s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos;
}
static bool Request(State state,const std::string& id,const std::string& name) {
    if(id.empty()||id.size()>96||!ValidName(name))return false;
    const auto path=MenuGame_SaveDir()+"/"+name;
    if(state==State::QueuedSave) {
        for(const char* suffix:{".sav",".sav.lua",".jpg"})if(access((path+suffix).c_str(),F_OK)==0)return false;
    } else if(access((path+".sav").c_str(),R_OK)!=0)return false;
    std::lock_guard<std::mutex> lock(C().mutex);
    if(!C().enabled||!g_progressMenu||C().state!=State::Idle||
       (state==State::QueuedSave&&!C().world))return false;
    C().state=state;C().operation=id;C().name=name;C().requestedWorld=C().world;
    return true;
}
// Called only at a genuine UI frame, before queuing operations on the engine.
static void Tick(void* menu,bool gameFrame) {
    State state;std::string id,name;uintptr_t world;
    const uintptr_t current=Field<uintptr_t>(g_base,RVA_G_GAMEUI);
    {
        std::lock_guard<std::mutex> lock(C().mutex);
        C().uiThread=Tid();C().world=current;
        if(!current)C().commandThread=0;
        state=C().state;world=C().requestedWorld;id=C().operation;name=C().name;
        if(state==State::Loading&&C().accepted&&gameFrame&&current&&
           !Field<uint8_t>(uintptr_t(menu),MENU_OFF_INITING)) {
            C().state=State::Idle;EmitLocked("world_ready",true);return;
        }
        if((state==State::Saving||state==State::Pausing||state==State::QueuedSave||state==State::QueuedPause)&&current!=world) {
            C().state=State::Idle;EmitLocked(state==State::Saving||state==State::QueuedSave?"saved":"paused",false,"World changed during operation");return;
        }
        if(state!=State::QueuedSave&&state!=State::QueuedPause&&state!=State::QueuedLoad)return;
        if(state==State::QueuedLoad && (Field<uint8_t>(uintptr_t(menu),MENU_OFF_INITING)||Field<uintptr_t>(uintptr_t(menu),MENU_OFF_QUEUED)))return;
        C().state=state==State::QueuedSave?State::Saving:state==State::QueuedPause?State::Pausing:State::Loading;
        C().accepted=false;
    }
    if(state==State::QueuedPause) {
        if(!SetActionsHeld(true)){Finish("paused",false,"Input gesture is active");return;}
        PauseNow(world,id,0);return;
    }
    if(state==State::QueuedSave) {
        if(Field<uint8_t>(world,GAMEUI_OFF_SAVING)){Finish("saved",false,"A save is already running");return;}
        SaveCall call{};call.world=world;call.name.p=call.name.buf;call.name.len=name.size();memcpy(call.name.buf,name.c_str(),name.size()+1);
        const bool threw=tpf2mp_mg_guarded(&SaveBody,&call)!=0;ownSave=nullptr;
        if(threw||!call.captured)Finish("saved",false,threw?"Engine save threw":"Engine did not queue save");
        return;
    }
    LoadCtx ctx{};ctx.menu=menu;ctx.name=name.c_str();ctx.result=-1;
    t_alInFlight=1;
    const bool threw=tpf2mp_mg_guarded(&LoadBody,&ctx)!=0;
    if(threw)tpf2mp_mg_guarded(&LoadCleanUp,&ctx);
    t_alInFlight=0;
    {
        std::lock_guard<std::mutex> lock(C().mutex);
        C().accepted=!threw&&ctx.result==1;
        if(!C().accepted)C().state=State::Idle;
        EmitLocked("load_accepted",C().accepted,C().accepted?"":"Engine refused load");
    }
}
static bool Install() {
    const uint8_t factory[]={0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x57,0x49,0x89,0xcf,0x41,0x56};
    const uint8_t done[]={0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x54,0x53,0x48,0x89,0xfb};
    const uint8_t legacy[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x49,0x89,0xf5,0x41,0x54};
    if(memcmp(reinterpret_cast<void*>(g_base+0x15ed140),factory,sizeof(factory))||
       memcmp(reinterpret_cast<void*>(g_base+0x1019f40),done,sizeof(done))||
       memcmp(reinterpret_cast<void*>(g_base+0x1d86ff0),legacy,sizeof(legacy)))return false;
    if(!InstallHook(g_base+0x15ed140,reinterpret_cast<void*>(&SaveFactoryHook),sizeof(factory),&saveFactoryOriginal))return false;
    if(!InstallHook(g_base+0x1019f40,reinterpret_cast<void*>(&SaveDoneHook),sizeof(done),&saveDoneOriginal))return false;
    if(!InstallHook(g_base+0x1d86ff0,reinterpret_cast<void*>(&LegacyEventHook),sizeof(legacy),&legacyEventOriginal))return false;
    C().enabled=true;return true;
}
}
bool Save(const std::string& id,const std::string& name){return Request(State::QueuedSave,id,name);}
bool Load(const std::string& id,const std::string& name){return Request(State::QueuedLoad,id,name);}
bool PauseAndDrain(const std::string& id) {
    std::lock_guard<std::mutex> lock(C().mutex);
    if(!C().enabled||!C().world||C().state!=State::Idle||id.empty()||id.size()>96)return false;
    C().state=State::QueuedPause;C().operation=id;C().requestedWorld=C().world;return true;
}
bool Poll(Event& e){std::lock_guard<std::mutex> lock(C().mutex);if(C().events.empty())return false;e=C().events.front();C().events.pop_front();return true;}
bool HasWorld(){std::lock_guard<std::mutex> lock(C().mutex);return C().world!=0;}
bool Busy(){std::lock_guard<std::mutex> lock(C().mutex);return C().state!=State::Idle;}
bool Loading(){std::lock_guard<std::mutex> lock(C().mutex);return C().state==State::Loading||C().state==State::QueuedLoad;}
bool SetActionsHeld(bool held){if(!panel::SetActionsHeld(held))return false;actionsHeld=held;return true;}
void WorkThreads(unsigned& ui,unsigned& command){std::lock_guard<std::mutex> lock(C().mutex);ui=C().uiThread;command=C().commandThread;}
}
