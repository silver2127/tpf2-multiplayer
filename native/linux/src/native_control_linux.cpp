#include "native_io_linux.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

namespace NativeControl {
namespace {
std::atomic<bool> running{false};
// The controller and game hooks live until process exit. Avoid destructing
// their storage while the detached polling thread is finishing its last tick.
std::string& directory=*new std::string;
std::string& initial=*new std::string;
bool supported;
std::string Clean(std::string s) { for(char& c:s) if(c=='\n'||c=='\r'||!c)c=' '; return s; }
std::map<std::string,std::string> Read(const char* name) {
    std::map<std::string,std::string> result;
    const int fd=open((directory+name).c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    if(fd<0)return result;
    struct stat st{}; char buffer[4097]; ssize_t count=-1;
    if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size<=4096) count=read(fd,buffer,sizeof(buffer));
    close(fd);
    if(count<=0||buffer[count-1]!='\n')return result;
    std::istringstream input(std::string(buffer,size_t(count))); std::string line;
    while(std::getline(input,line)) {
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        const auto at=line.find('=');
        if(at!=std::string::npos)result[line.substr(0,at)]=line.substr(at+1);
    }
    return result;
}
bool Write(const char* name,const std::string& text) {
    const std::string path=directory+name,tmp=path+".tmp";
    FILE* f=fopen(tmp.c_str(),"wb"); if(!f)return false;
    const size_t n=fwrite(text.data(),1,text.size(),f);
    const bool closed=fclose(f)==0;
    return n==text.size()&&closed&&rename(tmp.c_str(),path.c_str())==0;
}
uint64_t Cpu(unsigned tid) {
    if(!tid)return 0;
    std::ifstream f("/proc/self/task/"+std::to_string(tid)+"/stat"); std::string line;
    if(!std::getline(f,line))return 0;
    const auto end=line.rfind(')'); if(end==std::string::npos)return 0;
    std::istringstream fields(line.substr(end+2)); std::string ignored;
    for(int field=3;field<14;++field)if(!(fields>>ignored))return 0;
    uint64_t user=0,kernel=0; fields>>user>>kernel;
    const long hz=sysconf(_SC_CLK_TCK);
    return hz>0 ? (user+kernel)*1000/uint64_t(hz) : 0;
}
std::string Liveness() {
    unsigned ui=0,command=0; NativeIo::WorkThreads(ui,command);
    auto counters=[](const std::string& path,uint64_t& read,uint64_t& written) {
        std::ifstream f(path); std::string key; uint64_t value;
        while(f>>key>>value) {if(key=="rchar:")read=value;if(key=="wchar:")written=value;}
    };
    uint64_t read=0,written=0,ownRead=0,ownWritten=0;
    counters("/proc/self/io",read,written);
    counters("/proc/self/task/"+std::to_string(syscall(SYS_gettid))+"/io",ownRead,ownWritten);
    // Exclude this mailbox thread's reads, writes and diagnostic sampling.
    // Otherwise its heartbeat alone would keep a blocked engine 'alive'.
    read=read>ownRead?read-ownRead:0; written=written>ownWritten?written-ownWritten:0;
    return "\ncpu_ui="+std::to_string(Cpu(ui))+"\ncpu_command="+std::to_string(Cpu(command))+
        "\nio_read="+std::to_string(read)+"\nio_write="+std::to_string(written);
}
uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void Work() {
    const auto pid=std::to_string(getpid());
    std::string last=initial,pending,event,eventId; uint64_t deadline=0;
    auto emit=[&](const NativeIo::Event& e) {
        eventId=e.operation;
        event="pid="+pid+"\nid="+Clean(e.operation)+"\nstep="+e.step+
            "\nsuccess="+(e.success?"1":"0")+"\ndetail="+Clean(e.detail)+"\n";
    };
    while(running) {
        auto r=Read("tpf2_native_request.txt");
        if(r["pid"]==pid&&!r["id"].empty()&&r["id"].size()<=96&&r["id"]!=last) {
            last=r["id"]; event.clear(); eventId.clear(); pending.clear();
            bool ok=false,immediate=false;
            if(supported) {
                if(r["cmd"]=="save")ok=NativeIo::Save(last,r["name"]);
                else if(r["cmd"]=="load")ok=NativeIo::Load(last,r["name"]);
                else if(r["cmd"]=="pause")ok=NativeIo::PauseAndDrain(last);
                else if(r["cmd"]=="hold") {pending=last;deadline=Now()+10000;ok=true;}
                else if(r["cmd"]=="release") {immediate=true;ok=NativeIo::SetActionsHeld(false);}
            }
            if(!ok||immediate)emit({last,immediate?"held":"request",ok?"":"Native request refused",ok});
        }
        if(!pending.empty()) {
            const bool held=NativeIo::SetActionsHeld(true);
            if(held||Now()>=deadline) {emit({pending,"held",held?"":"Input gesture did not finish",held});pending.clear();}
        }
        NativeIo::Event e;
        while(NativeIo::Poll(e))if(e.operation==last)emit(e);
        const bool published=event.empty()||Write("tpf2_native_event.txt",event);
        Write("tpf2_native_status.txt","pid="+pid+"\nsupported="+(supported?"1":"0")+
              "\nhas_world="+(NativeIo::HasWorld()?"1":"0")+"\nbusy="+(NativeIo::Busy()?"1":"0")+
              "\nlast_request="+Clean(last)+"\nlast_event="+Clean(eventId)+
              "\nevent_published="+(published?"1":"0")+Liveness()+"\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
}
void Start(const std::string& path,bool ready) {
    if(running.exchange(true))return;
    directory=path; if(!directory.empty()&&directory.back()!='/')directory+='/';
    supported=ready; initial=Read("tpf2_native_request.txt")["id"];
    try {std::thread(Work).detach();}catch(...){running=false;}
}
void SignalShutdown(){running=false;}
}
