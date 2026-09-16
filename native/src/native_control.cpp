#include "native_control.h"
#include "native_io.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <cstdio>

namespace NativeControl {
namespace {
std::atomic<bool> running{false};
std::wstring directory;
bool supported=false;
std::string latestEvent, latestEventId;
std::string initialRequest;
uint64_t ownWrites=0;        // bytes this thread wrote to the mailbox files (netted out of io_write)
std::string clean(std::string text) {
    for(auto& c:text) if(c=='\r' || c=='\n' || c=='\0') c=' ';
    return text;
}
std::map<std::string,std::string> read(const wchar_t* name) {
    std::map<std::string,std::string> result;
    HANDLE file=CreateFileW((directory+name).c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE) return result;
    char buffer[4097]{}; DWORD count=0;
    const bool ok=ReadFile(file,buffer,4096,&count,nullptr)!=0;
    CloseHandle(file);
    if(!ok || !count || buffer[count-1]!='\n') return result;
    std::string text(buffer,count); size_t start=0;
    while(start<text.size()) {
        auto end=text.find('\n',start), equal=text.find('=',start);
        if(equal<end) {
            auto value=text.substr(equal+1,end-equal-1);
            if(!value.empty() && value.back()=='\r') value.pop_back();
            result[text.substr(start,equal-start)]=value;
        }
        start=end+1;
    }
    return result;
}
bool write(const wchar_t* name,const std::string& text) {
    const auto path=directory+name, temporary=path+L".tmp";
    FILE* file=nullptr; _wfopen_s(&file,temporary.c_str(),L"wb");
    if(!file) return false;
    const bool ok=fwrite(text.data(),1,text.size(),file)==text.size();
    if(ok) ownWrites+=text.size();
    const bool closed=fclose(file)==0;
    return ok && closed && MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
// Whether a save or load is ALIVE cannot be read from this thread's own
// heartbeat (it ticks whether or not the engine advances -- a game thread hung
// mid-save kept the lobby's silence timeout from ever firing, 2026-09-16). What
// can be read from outside the engine is the work itself: CPU time burnt by
// the thread doing it (GetThreadTimes, kernel+user, in ms) and the bytes the
// process moved through the disk (its IO counters, writes net of this
// mailbox's own). A deadlocked or dialog-blocked thread burns none and moves
// none; the lobby quantises these so that idle noise never reads as progress
// (sync_runtime.engine_work).
struct ThreadClock { DWORD id=0; HANDLE handle=nullptr; };
ThreadClock uiClock, commandClock;
uint64_t cpuMilliseconds(ThreadClock& clock,DWORD id) {
    if(clock.id!=id) {
        if(clock.handle) CloseHandle(clock.handle);
        clock.handle=id ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,id) : nullptr;
        clock.id=id;
    }
    FILETIME created{},exited{},kernel{},user{};
    if(!clock.handle || !GetThreadTimes(clock.handle,&created,&exited,&kernel,&user)) return 0;
    ULARGE_INTEGER k{{kernel.dwLowDateTime,kernel.dwHighDateTime}}, u{{user.dwLowDateTime,user.dwHighDateTime}};
    return (k.QuadPart+u.QuadPart)/10000;
}
std::string liveness() {
    DWORD uiThread=0,command=0;
    NativeIo::WorkThreads(uiThread,command);
    IO_COUNTERS io{};
    if(!GetProcessIoCounters(GetCurrentProcess(),&io)) io=IO_COUNTERS{};
    const uint64_t written=io.WriteTransferCount>ownWrites ? io.WriteTransferCount-ownWrites : 0;
    return "\ncpu_ui="+std::to_string(cpuMilliseconds(uiClock,uiThread))+
        "\ncpu_command="+std::to_string(cpuMilliseconds(commandClock,command))+
        "\nio_read="+std::to_string(io.ReadTransferCount)+"\nio_write="+std::to_string(written);
}
void event(const NativeIo::Event& value) {
    latestEventId=value.operation;
    latestEvent="pid="+std::to_string(GetCurrentProcessId())+
        "\nid="+clean(value.operation)+"\nstep="+value.step+"\nsuccess="+(value.success?"1":"0")+
        "\ndetail="+clean(value.detail)+"\n";
}
DWORD WINAPI work(void*) {
    std::string last = initialRequest;
    std::string pendingHold;
    ULONGLONG holdDeadline=0;
    const auto pid=std::to_string(GetCurrentProcessId());
    while(running.load()) {
        auto request=read(L"tpf2_native_request.txt");
        if(request["pid"]==pid && !request["id"].empty() && request["id"].size()<=96 && request["id"]!=last) {
            last=request["id"];
            latestEvent.clear(); latestEventId.clear();
            const auto& cmd=request["cmd"];
            bool ok=false, immediate=false;
            if(supported) {
                if(cmd=="save") ok=NativeIo::Save(last,request["name"]);
                else if(cmd=="load") ok=NativeIo::Load(last,request["name"]);
                else if(cmd=="pause") ok=NativeIo::PauseAndDrain(last);
                else if(cmd=="hold") {
                    // Wait for the initiating click/gesture to finish naturally.
                    // No synthetic release and no abandoned engine callback.
                    pendingHold=last; holdDeadline=GetTickCount64()+10000; ok=true;
                }
                else if(cmd=="release") { pendingHold.clear(); immediate=true; ok=NativeIo::SetActionsHeld(false); }
            }
            if(!ok || immediate) event({last,immediate?"held":"request",ok?"":"Native request refused",ok});
        }
        if(!pendingHold.empty()) {
            const bool held=NativeIo::SetActionsHeld(true);
            if(held || GetTickCount64()>=holdDeadline) {
                event({pendingHold,"held",held?"":"Input gesture did not finish",held});
                pendingHold.clear();
            }
        }
        NativeIo::Event value;
        while(NativeIo::Poll(value)) if(value.operation==last) event(value);
        // Readers can momentarily deny rename on Windows. Retain/re-publish the
        // final acknowledgement until a later request supersedes it; never lose
        // a completed save/pause just because one atomic replace failed.
        const bool published=latestEvent.empty() || write(L"tpf2_native_event.txt",latestEvent);
        write(L"tpf2_native_status.txt","pid="+pid+"\nsupported="+(supported?"1":"0")+
            "\nhas_world="+(NativeIo::HasWorld()?"1":"0")+"\nbusy="+(NativeIo::Busy()?"1":"0")+
            "\nlast_request="+last+"\nlast_event="+latestEventId+"\nevent_published="+(published?"1":"0")+liveness()+"\n");
        Sleep(100);
    }
    return 0;
}
}
void Start(const std::wstring& path,bool ready) {
    if(running.exchange(true)) return;
    directory=path;
    if(!directory.empty() && directory.back()!=L'\\' && directory.back()!=L'/') directory+=L'\\';
    supported=ready;
    // Ignore a mailbox left by an earlier process, even if Windows reused its
    // PID. Capture this before starting the worker so a fresh request cannot race it.
    initialRequest=read(L"tpf2_native_request.txt")["id"];
    HANDLE thread=CreateThread(nullptr,0,work,nullptr,0,nullptr);
    if(thread) CloseHandle(thread); else running=false;
}
void SignalShutdown() { running=false; }
}
