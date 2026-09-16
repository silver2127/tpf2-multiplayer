"""Compile production native IPC; force a Windows rename-sharing conflict.

The engine is a stub. This specifically tests acknowledgement persistence, not
native engine save/load correctness.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.local-test/tests/native-control'
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <windows.h>
#include <atomic>
#include <cassert>
#include <fstream>
#include <iterator>
#include <queue>
#include <mutex>
#include "NATIVE_IO"
#include "NATIVE_CONTROL"
std::atomic<int> pauses{0};
std::mutex eventMutex;
std::queue<NativeIo::Event> events;
namespace NativeIo {
bool Save(const std::string&,const std::string&) { return false; }
bool Load(const std::string&,const std::string&) { return false; }
bool PauseAndDrain(const std::string& id) {
    ++pauses; std::lock_guard<std::mutex> l(eventMutex);
    events.push({id,"paused","",true}); return true;
}
bool Poll(Event& e) { std::lock_guard<std::mutex> l(eventMutex); if(events.empty())return false; e=events.front();events.pop();return true; }
bool HasWorld(){return true;} bool Busy(){return false;} bool SetActionsHeld(bool){return true;}
void WorkThreads(DWORD& ui,DWORD& command){ui=GetCurrentThreadId();command=0;}
}
#include "CONTROL_CPP"
std::string read(const char* path) {std::ifstream f(path);return std::string(std::istreambuf_iterator<char>(f),{});}
template<class F> void waitFor(F f) {for(int i=0;i<100;++i){if(f())return;Sleep(20);}assert(false);}
int main() {
    const char* event="tpf2_native_event.txt";
    {std::ofstream f(event);f<<"old acknowledgement\n";}
    HANDLE held=CreateFileA(event,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    assert(held!=INVALID_HANDLE_VALUE);
    {std::ofstream f("tpf2_native_request.txt",std::ios::binary);f<<"pid="<<GetCurrentProcessId()<<"\nid=stale\ncmd=pause\n";}
    NativeControl::Start(L".",true);
    Sleep(150); assert(pauses==0);
    {std::ofstream f("tpf2_native_request.txt",std::ios::binary);f<<"pid="<<GetCurrentProcessId()<<"\nid=once\ncmd=pause\n";}
    waitFor([]{return pauses==1;});
    Sleep(300);
    assert(read(event).find("old acknowledgement")!=std::string::npos);
    assert(read("tpf2_native_status.txt").find("event_published=0")!=std::string::npos);
    // the liveness fields the lobby reads: this thread's CPU time (the stub's
    // "UI thread"), no command thread yet, the process's IO counters
    { auto status=read("tpf2_native_status.txt");
      assert(status.find("\ncpu_ui=")!=std::string::npos && status.find("\ncpu_command=0\n")!=std::string::npos);
      assert(status.find("\nio_read=")!=std::string::npos && status.find("\nio_write=")!=std::string::npos); }
    CloseHandle(held);
    waitFor([&]{return read(event).find("step=paused\nsuccess=1")!=std::string::npos;});
    assert(pauses==1); // completion was repeated, never the engine command
    NativeControl::SignalShutdown(); Sleep(200);
    puts("PASS: native completion survives Windows sharing violation without repeating engine command");
}
'''
code = code.replace('NATIVE_IO', (root/'native/src/native_io.h').as_posix())
code = code.replace('NATIVE_CONTROL', (root/'native/src/native_control.h').as_posix())
code = code.replace('CONTROL_CPP', (root/'native/src/native_control.cpp').as_posix())
(out/'test.cpp').write_text(code)
vcvars = root / 'tools' / 'msvc_env.bat'
(out/'build.cmd').write_text(f'@echo off\ncall "{vcvars}" || exit /b 1\n'
    'cl /nologo /EHsc /W4 test.cpp /Fe:test.exe >build.log 2>&1\n'
    'if errorlevel 1 (type build.log & exit /b 1)\nexit /b 0\n')
subprocess.run(['cmd','/d','/c',str(out/'build.cmd')], cwd=out, check=True, timeout=180)  # absolute: a relative name fails on some shells
subprocess.run([str(out / 'test.exe')], cwd=out, check=True, timeout=20)
