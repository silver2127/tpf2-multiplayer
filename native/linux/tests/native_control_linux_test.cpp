#include "../src/native_control_linux.cpp"
#include <cassert>
#include <sys/wait.h>
namespace NativeIo {
bool Save(const std::string&,const std::string&){return false;}
bool Load(const std::string&,const std::string&){return false;}
bool PauseAndDrain(const std::string&){return false;}
bool Poll(Event&){return false;}
bool HasWorld(){return false;}
bool Busy(){return false;}
std::atomic<int> gesture{0};
bool SetActionsHeld(bool held){return !held || !gesture.load();}
int ActiveGestureKey(){return gesture.load();}
void WorkThreads(unsigned& ui,unsigned& command){ui=command=0;}
}
int main() {
    for (bool same : {true,false}) {
        const auto child=fork();assert(child>=0);
        if(!child) {
            char temp[]="/tmp/tpf2-control-XXXXXX";assert(mkdtemp(temp));
            const std::string dir=std::string(temp)+"/", path=dir+"tpf2_sync_lua.txt";
            const std::string stale="phase=error\nresume_speed=0\npid="+std::to_string(getpid()+(same?0:1))+"\n";
            {std::ofstream f(path);f<<stale;}
            NativeControl::Start(dir,false);
            {std::ifstream f(path);std::string got((std::istreambuf_iterator<char>(f)),{});assert(got==(same?"":stale));}
            NativeControl::SignalShutdown();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            for(const char* name:{"tpf2_sync_lua.txt","tpf2_native_status.txt"})unlink((dir+name).c_str());
            assert(rmdir(temp)==0);_exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    char temp[]="/tmp/tpf2-hold-XXXXXX";assert(mkdtemp(temp));
    const std::string dir=std::string(temp)+"/";
    auto read=[&](const char* name){std::ifstream f(dir+name);return std::string((std::istreambuf_iterator<char>(f)),{});};
    auto wait=[&](const char* name,const std::string& text){
        for(int i=0;i<50;++i){if(read(name).find(text)!=std::string::npos)return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));}
        assert(false);
    };
    auto request=[&](const char* id,const char* cmd){
        std::ofstream f(dir+"request.tmp");f<<"pid="<<getpid()<<"\nid="<<id<<"\ncmd="<<cmd<<"\n";f.close();
        assert(rename((dir+"request.tmp").c_str(),(dir+"tpf2_native_request.txt").c_str())==0);
    };
    NativeControl::Start(dir,true);
    NativeIo::gesture=4;request("long","hold");
    wait("tpf2_native_status.txt","\nhold_key=4\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(10500));
    assert(read("tpf2_native_event.txt").find("id=long")==std::string::npos);
    NativeIo::gesture=0;
    wait("tpf2_native_event.txt","id=long\nstep=held\nsuccess=1");
    wait("tpf2_native_status.txt","\nhold_key=0\n");
    NativeIo::gesture=-1;request("cancelled","hold");
    wait("tpf2_native_status.txt","\nhold_key=-1\n");
    request("release","release");
    wait("tpf2_native_event.txt","id=release\nstep=held\nsuccess=1");
    wait("tpf2_native_status.txt","\nhold_key=0\n");
    NativeIo::gesture=0;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(read("tpf2_native_event.txt").find("id=release")!=std::string::npos);
    NativeControl::SignalShutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for(const char* name:{"tpf2_native_request.txt","tpf2_native_status.txt","tpf2_native_event.txt"})unlink((dir+name).c_str());
    assert(rmdir(temp)==0);

}
