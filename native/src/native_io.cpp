#include "native_io.h"
#include "hook.h"
#include <wincrypt.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

namespace NativeIo {
namespace {
uintptr_t base;
HMODULE module;
std::wstring directory;
std::mutex mutex;
std::deque<Event> events;
enum class State { Idle, QueuedSave, QueuedLoad, QueuedPause, Saving, Loading, Pausing };
State state = State::Idle;
std::string operation, name;
uintptr_t menu = 0, ui = 0;
DWORD owner = 0;
HHOOK pumpHook = nullptr;
std::atomic<bool> enabled{false}, initializationAttempted{false};
bool accepted = false;
std::atomic<bool> actionsHeld{false};
WNDPROC originalWindowProc=nullptr;
HWND inputWindow=nullptr;
const UINT pumpMessage = WM_APP + 0x392;

LRESULT CALLBACK inputProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(actionsHeld.load()) {
        switch(message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        case WM_CHAR: case WM_SYSCHAR:
            if(w!=VK_ESCAPE) return 0;
            break;
        // The game also observes the button state in mouse-move messages.
        // Filtering only button-down lets toolbar actions through in engine
        // tests. Enter the hold only after physical buttons/keys are released.
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        case WM_XBUTTONUP:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            return 0;
        }
    }
    return CallWindowProcW(originalWindowProc,window,message,w,l);
}
BOOL CALLBACK findInputWindow(HWND window,LPARAM) {
    DWORD process=0;
    GetWindowThreadProcessId(window,&process);
    if(process!=GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window,GW_OWNER)) return TRUE;
    wchar_t title[128]{}; GetWindowTextW(window,title,128);
    if(wcscmp(title,L"Transport Fever 2")!=0) return TRUE;
    inputWindow=window;
    SetLastError(0);
    auto previous=SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&inputProc));
    if(previous) originalWindowProc=reinterpret_cast<WNDPROC>(previous);
    else inputWindow=nullptr;
    return FALSE;
}

template<class T> T fn(uintptr_t rva) { return reinterpret_cast<T>(base + rva); }
template<class T> T& field(uintptr_t object, uintptr_t offset) {
    return *reinterpret_cast<T*>(object + offset);
}
void emit(const std::string& id, const char* step, bool ok, const char* detail = "") {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back({id, step, detail, ok});
}
bool supportedImage() {
    wchar_t path[32768];
    if (!GetModuleFileNameW(nullptr,path,32768)) return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider=0; HCRYPTHASH hash=0;
    bool ok=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)!=0;
    if (ok) ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=0;
    BYTE buffer[65536]; DWORD count=0;
    while(ok) {
        if (!ReadFile(file,buffer,sizeof(buffer),&count,nullptr)) { ok=false; break; }
        if (!count) break;
        ok=CryptHashData(hash,buffer,count,0)!=0;
    }
    BYTE digest[32]; DWORD size=sizeof(digest);
    if(ok) ok=CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0)!=0;
    const BYTE expected[32]={0x78,0x2b,0x90,0x4a,0x8f,0x7b,0xbd,0xac,0x1f,0x7a,0x18,0x52,0x8f,0x1a,0x5c,0x77,0x86,0x91,0xe5,0xaa,0x30,0x87,0xc3,0x7c,0x35,0x1b,0xf6,0x91,0x25,0x85,0x17,0x5c};
    ok=ok && size==32 && memcmp(digest,expected,32)==0;
    if(hash) CryptDestroyHash(hash);
    if(provider) CryptReleaseContext(provider,0);
    CloseHandle(file); return ok;
}
struct alignas(8) InlineString {
    char text[16]{}; uint64_t size=0, capacity=15;
    explicit InlineString(const std::string& value) { memcpy(text,value.data(),value.size()); size=value.size(); }
};
static_assert(sizeof(InlineString)==32,"native string ABI");
struct Ticket { std::atomic<unsigned> refs{1}; std::string id; uintptr_t world; unsigned pass=0; };
struct FunctionObject { void** table; Ticket* ticket; };
struct SmallFunction { alignas(8) unsigned char storage[56]{}; FunctionObject* object=nullptr; };
static_assert(sizeof(SmallFunction)==64,"native function ABI");
FunctionObject* copyFunction(FunctionObject* source, void* destination) {
    ++source->ticket->refs;
    auto result=new(destination) FunctionObject{source->table,source->ticket};
    return result;
}
void dropFunction(FunctionObject* object, bool) {
    if(--object->ticket->refs==0) delete object->ticket;
}
void* noType(FunctionObject*) { return nullptr; }
void* noTarget(FunctionObject*,const void*) { return nullptr; }
void saveComplete(FunctionObject* object, const bool* success, const void*) {
    const auto ticket=object->ticket;
    bool current;
    {
        std::lock_guard<std::mutex> lock(mutex);
        current=ui==ticket->world && state==State::Saving && operation==ticket->id;
        if(current) {
            field<unsigned char>(ui,0xb48)=0;
            field<uint64_t>(ui,0x648)=0;
            state=State::Idle;
        }
    }
    emit(ticket->id,"saved",current && *success,current ? (*success ? "" : "Engine save failed") : "World changed during save");
}
void* functionTable[]={reinterpret_cast<void*>(&copyFunction),reinterpret_cast<void*>(&copyFunction),
    reinterpret_cast<void*>(&saveComplete),reinterpret_cast<void*>(&noType),
    reinterpret_cast<void*>(&dropFunction),reinterpret_cast<void*>(&noTarget)};

void pauseNow(uintptr_t world,const std::string& id,unsigned pass);
void pauseComplete(FunctionObject* object,const unsigned char* command) {
    const auto ticket=object->ticket;
    bool current, drained=false, success=command && command[0x30]!=0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        current=ui==ticket->world && state==State::Pausing && operation==ticket->id;
        if(current && success) {
            const auto queue=field<uintptr_t>(field<uintptr_t>(ui,0x448),0x160);
            const auto impl=field<uintptr_t>(queue,0);
            drained=field<uintptr_t>(impl,0)==field<uintptr_t>(impl,8);
        }
        if(current && (!success || drained || ticket->pass>=64)) state=State::Idle;
    }
    if(current && success && !drained && ticket->pass<64) {
        pauseNow(ticket->world,ticket->id,ticket->pass+1);
        return;
    }
    emit(ticket->id,"paused",current && success && drained,
         !current ? "World changed during pause" : !success ? "Engine pause failed" :
         !drained ? "Command producers did not become idle" : "");
}
void* pauseFunctionTable[]={reinterpret_cast<void*>(&copyFunction),reinterpret_cast<void*>(&copyFunction),
    reinterpret_cast<void*>(&pauseComplete),reinterpret_cast<void*>(&noType),
    reinterpret_cast<void*>(&dropFunction),reinterpret_cast<void*>(&noTarget)};

using LegacyScriptEvent=void(*)(uintptr_t,uintptr_t);
LegacyScriptEvent originalLegacyScriptEvent;
void legacyScriptEventHook(uintptr_t binding,uintptr_t luaState) {
    // This legacy void API has no user completion callback. Stop at its entry,
    // before native strings/commands/callback adapters exist. The vanilla guide
    // script otherwise queues saveevent every GUI frame, even in a paused world.
    if(actionsHeld.load()) return;
    originalLegacyScriptEvent(binding,luaState);
}
void pauseNow(uintptr_t world,const std::string& id,unsigned pass) {
    alignas(16) unsigned char command[64];
    SmallFunction completion;
    uint64_t progress[2]{}, result[2]{};
    auto ticket=new Ticket; ticket->id=id; ticket->world=world; ticket->pass=pass;
    completion.object=new(completion.storage) FunctionObject{pauseFunctionTable,ticket};
    fn<void*(*)(void*,int)>(0x9de9e0)(command,0);
    const auto queue=field<uintptr_t>(field<uintptr_t>(world,0x448),0x160);
    fn<void(*)(uintptr_t,void*,void*,void*,void*)>(0x9d2a00)(queue,result,command,&completion,progress);
    fn<void(*)(void*)>(0x2357910)(result);
}

void saveNow(uintptr_t world,const std::string& id,const std::string& basename) {
    // All nontrivial engine-owned objects are constructed and consumed by the
    // engine, including their strings, vectors and asynchronous command state.
    alignas(16) unsigned char metadata[176], picture[32]{}, context[248], command[64];
    SmallFunction completion, adapter;
    uint64_t progress[2]{}, result[2]{};
    InlineString filename(basename);
    auto ticket=new Ticket; ticket->id=id; ticket->world=world;
    completion.object=new(completion.storage) FunctionObject{functionTable,ticket};
    fn<void(*)(uintptr_t,void*)>(0x5647b0)(world,metadata);
    *reinterpret_cast<int*>(picture)=640;
    *reinterpret_cast<int*>(picture+4)=360;
    fn<void(*)(uintptr_t,uintptr_t,void*,void*,void*)>(0xbf9b40)(
        field<uintptr_t>(field<uintptr_t>(world,0x18),0x98),
        field<uintptr_t>(field<uintptr_t>(world,0x5e8),0x448),picture,picture+4,picture+8);
    adapter.object=fn<FunctionObject*(*)(void*)>(0x5467e0)(&completion);
    auto game=field<uintptr_t>(world,0x448);
    fn<void*(*)(void*,uintptr_t)>(0x2e4e40)(context,game);
    fn<void*(*)(void*,void*,void*,void*,void*,bool,bool)>(0x9de0e0)(command,context,metadata,&filename,picture,false,false);
    field<unsigned char>(world,0xb48)=1;
    field<uint64_t>(world,0x648)=0;
    fn<void(*)(uintptr_t,void*,void*,void*,void*)>(0x9d2a00)(field<uintptr_t>(game,0x160),result,command,&adapter,progress);
    fn<void(*)(void*)>(0x2357910)(result);
}

using StartFn=bool(*)(uintptr_t,void*,void*);
StartFn originalStart;
bool startHook(uintptr_t target,void* params,void* info) {
    const bool result=originalStart(target,params,info);
    std::lock_guard<std::mutex> lock(mutex);
    if(state==State::Loading && target==menu) accepted=result;
    return result;
}
// All 26 parameters are pointers/references or MSVC indirect by-value objects.
// No variadic forwarding: preserve the complete constructor ABI explicitly.
#define CTOR_ARGS uintptr_t a0,uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t a4,uintptr_t a5,uintptr_t a6,uintptr_t a7,uintptr_t a8,uintptr_t a9,uintptr_t a10,uintptr_t a11,uintptr_t a12,uintptr_t a13,uintptr_t a14,uintptr_t a15,uintptr_t a16,uintptr_t a17,uintptr_t a18,uintptr_t a19,uintptr_t a20,uintptr_t a21,uintptr_t a22,uintptr_t a23,uintptr_t a24,uintptr_t a25
#define CTOR_VALUES a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20,a21,a22,a23,a24,a25
using Constructor=uintptr_t(*)(CTOR_ARGS);
Constructor originalConstructor;
uintptr_t constructorHook(CTOR_ARGS) {
    auto result=originalConstructor(CTOR_VALUES);
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex);
        ui=a0;
        if(state==State::Loading && accepted) { id=operation; state=State::Idle; }
    }
    if(!id.empty()) emit(id,"world_ready",true);
    return result;
}
using Destructor=uintptr_t(*)(uintptr_t,unsigned);
Destructor originalDestructor;
uintptr_t destructorHook(uintptr_t world,unsigned flags) {
    std::string failed;
    const char* failedStep="saved";
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(ui==world) {
            ui=0;
            if(state==State::Saving || state==State::QueuedSave || state==State::Pausing || state==State::QueuedPause) {
                failedStep=(state==State::Pausing || state==State::QueuedPause) ? "paused" : "saved";
                failed=operation; state=State::Idle;
            }
        }
    }
    if(!failed.empty()) emit(failed,failedStep,false,"World destroyed during native operation");
    return originalDestructor(world,flags);
}
void execute() {
    State request; uintptr_t world,target; std::string id,basename;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(GetCurrentThreadId()!=owner) return;
        request=state; world=ui; target=menu; id=operation; basename=name;
        if(request!=State::QueuedLoad && request!=State::QueuedSave && request!=State::QueuedPause) return;
        state=request==State::QueuedSave ? State::Saving : request==State::QueuedPause ? State::Pausing : State::Loading;
        accepted=false;
    }
    if(request==State::QueuedPause) {
        if(!world || !SetActionsHeld(true)) {
            { std::lock_guard<std::mutex> lock(mutex); state=State::Idle; }
            emit(id,"paused",false,"No world or an input gesture is still active"); return;
        }
        pauseNow(world,id,0); return;
    }
    if(request==State::QueuedSave && (!world || field<unsigned char>(world,0xb48))) {
        { std::lock_guard<std::mutex> lock(mutex); state=State::Idle; }
        emit(id,"saved",false,"No idle world available"); return;
    }
    if(request==State::QueuedSave) { saveNow(world,id,basename); return; }
    InlineString filename(basename);
    uintptr_t capture[]={target,field<uintptr_t>(base,0x4332b68)};
    fn<bool(*)(void*,void*)>(0xc158f0)(capture,&filename);
    bool ok;
    { std::lock_guard<std::mutex> lock(mutex); ok=accepted; if(!ok) state=State::Idle; }
    emit(id,"load_accepted",ok,ok ? "" : "Engine refused load");
}
LRESULT CALLBACK pump(int code,WPARAM w,LPARAM l) {
    if(code>=0 && w==PM_REMOVE) {
        auto msg=reinterpret_cast<MSG*>(l);
        if(msg->message==pumpMessage && msg->wParam==reinterpret_cast<WPARAM>(module)) {
            msg->message=WM_NULL;
            execute();
        }
    }
    return CallNextHookEx(pumpHook,code,w,l);
}
bool request(State wanted,const std::string& id,const std::string& basename) {
    if(id.empty() || id.size()>96 || basename.size()<4 || basename.size()>15 || basename.compare(0,3,"mp_")!=0 || basename.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if(!enabled || !pumpHook || !menu || state!=State::Idle || (wanted==State::QueuedSave && !ui)) return false;
    std::wstring path=directory+L"\\"+std::wstring(basename.begin(),basename.end());
    if(wanted==State::QueuedSave) {
        for(auto suffix:{L".sav",L".sav.lua",L".jpg"})
            if(GetFileAttributesW((path+suffix).c_str())!=INVALID_FILE_ATTRIBUTES) return false;
    } else if(GetFileAttributesW((path+L".sav").c_str())==INVALID_FILE_ATTRIBUTES) return false;
    state=wanted; operation=id; name=basename;
    if(!PostThreadMessageW(owner,pumpMessage,reinterpret_cast<WPARAM>(module),0)) { state=State::Idle; return false; }
    return true;
}
}

bool Initialize(uintptr_t image,HMODULE self,const wchar_t* saves) {
    // Never stack detours when an initialization attempt failed partway.
    if(initializationAttempted.exchange(true)) return enabled.load();
    base=image; module=self; directory=saves;
    if(!supportedImage()) return false;
    void* constructor=nullptr; void* destructor=nullptr; void* start=nullptr; void* scriptEvent=nullptr;
    if(!InstallHook(base+0x54ea60,reinterpret_cast<void*>(&constructorHook),20,&constructor)) return false;
    originalConstructor=reinterpret_cast<Constructor>(constructor);
    if(!InstallHook(base+0x562210,reinterpret_cast<void*>(&destructorHook),15,&destructor)) return false;
    originalDestructor=reinterpret_cast<Destructor>(destructor);
    if(!InstallHook(base+0x6785c0,reinterpret_cast<void*>(&startHook),20,&start)) return false;
    originalStart=reinterpret_cast<StartFn>(start);
    if(!InstallHook(base+0x1126cd0,reinterpret_cast<void*>(&legacyScriptEventHook),21,&scriptEvent)) return false;
    originalLegacyScriptEvent=reinterpret_cast<LegacyScriptEvent>(scriptEvent);
    enabled=true; return true;
}
void ObserveMenu(uintptr_t target) {
    std::lock_guard<std::mutex> lock(mutex);
    menu=target;
    if(!enabled) return;
    if(!pumpHook) {
        owner=GetCurrentThreadId();
        pumpHook=SetWindowsHookExW(WH_GETMESSAGE,pump,module,owner);
    }
    if(!inputWindow) EnumThreadWindows(owner,findInputWindow,0);
}
bool Save(const std::string& id,const std::string& basename) { return request(State::QueuedSave,id,basename); }
bool Load(const std::string& id,const std::string& basename) { return request(State::QueuedLoad,id,basename); }
bool PauseAndDrain(const std::string& id) {
    if(id.empty() || id.size()>96) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if(!enabled || !pumpHook || !ui || state!=State::Idle) return false;
    state=State::QueuedPause; operation=id; name.clear();
    if(!PostThreadMessageW(owner,pumpMessage,reinterpret_cast<WPARAM>(module),0)) { state=State::Idle; return false; }
    return true;
}
bool Poll(Event& event) { std::lock_guard<std::mutex> lock(mutex); if(events.empty()) return false; event=events.front(); events.pop_front(); return true; }
bool HasWorld() { std::lock_guard<std::mutex> lock(mutex); return ui!=0; }
bool Busy() { std::lock_guard<std::mutex> lock(mutex); return state!=State::Idle; }
bool SetActionsHeld(bool held) {
    std::lock_guard<std::mutex> lock(mutex);
    if(held && !inputWindow) return false;
    if(held && !actionsHeld.load()) {
        // Fail without changing the gate while a previous gesture is active.
        // The coordinator must retry, then drain engine commands before saving.
        for(int key=1;key<256;++key)
            if(key!=VK_ESCAPE && (GetAsyncKeyState(key)&0x8000)) return false;
    }
    actionsHeld.store(held); return true;
}
}
