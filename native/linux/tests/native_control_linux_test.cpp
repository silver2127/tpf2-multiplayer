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
bool SetActionsHeld(bool){return false;}
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
}
