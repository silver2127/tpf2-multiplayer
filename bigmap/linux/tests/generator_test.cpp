#include "../../../native/src/plugin/tpf2mp_plugin.h"
#include "../generator_memory.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
using namespace linux_generator;
static std::string Read(FILE* f){assert(f);std::string s;char b[4096];size_t n;while((n=fread(b,1,sizeof b,f)))s.append(b,n);fclose(f);return s;}
static int enabled=1,verify=1,writes=0,patchOk=1;
static uint8_t installed[16];
static int Cfg(const char*,const char*,int){return enabled;}
static int CfgInt(const char*,const char* key,int fallback){assert(!strcmp(key,"generator_memory_budget_pct") && fallback==50);return 0;}
static int Verify(uintptr_t r,const uint8_t* b,uint32_t n){assert(r==FopenPlt && n==16 && !memcmp(b,FopenBytes,n));return verify;}
static int Patch(uintptr_t r,const uint8_t* b,uint32_t n){assert(r==FopenPlt && n==16 && b[0]==0xff && b[1]==0x25);++writes;memcpy(installed,b,n);return patchOk;}
static void Log(const char*,...){}
extern "C" __attribute__((visibility("default"))) long long TestPatch(const char* src,size_t len,char* out,size_t cap,unsigned long long budget=0){size_t n=0;char* p=PatchGeneratorText(src,len,&n,budget);if(!p)return -1;if(out && cap>=n)memcpy(out,p,n);free(p);return n;}
int main(){
    Tpf2mpHost h{};h.cfgBool=Cfg;h.cfgInt=CfgInt;h.verifyBytes=Verify;h.patchBytes=Patch;h.log=Log;
    enabled=0;assert(!Install(&h) && writes==0);enabled=1;verify=0;assert(!Install(&h) && writes==0);verify=1;patchOk=0;assert(!Install(&h) && writes==1);patchOk=1;assert(Install(&h) && writes==2);host=nullptr;
    for(const char* bad:{"","x","\t\treturn result -- no\n","x\t\treturn result\n","\t\treturn result\n\t\treturn result\n"})assert(TestPatch(bad,strlen(bad),nullptr,0)==-1);
    for(const char* good:{"\t\treturn result","\t\treturn result\n","\t\treturn result\r\n"})assert(TestPatch(good,strlen(good),nullptr,0)>0);
    assert(budgetPct==0);
    assert(Budget(10000,6000,2000,2,50)==2000);
    assert(Budget(10000,6000,2000,0,50)==5000);
    assert(Budget(10000,6000,2000,1,50)==5000);
    assert(Budget(10000,6000,7000,2,50)==0);
    assert(Budget(10000,6000,6000,2,50)==0);
    assert(Budget(10000,6000,2000,0,100)==9000);
    assert(Budget(10000,6000,2000,0,-1)==0);
    assert(Budget(UINT64_MAX,6000,2000,0,50)==0);
    assert(Budget(10000,UINT64_MAX,2000,2,50)==0);
    assert(Budget(UINT64_MAX-1,0,0,0,90)<UINT64_MAX);
    char root[]="generator-test-XXXXXX";assert(mkdtemp(root));
    auto info=std::filesystem::path(root)/"meminfo";
    auto mode=std::filesystem::path(root)/"mode";
    {std::ofstream f(info);f<<"MemAvailable: 10000 kB\nCommitLimit: 6000 kB\nCommitted_AS: 2000 kB\n";}
    {std::ofstream f(mode);f<<"2\n";}
    budgetPct=50;assert(BudgetBytes(info.c_str(),mode.c_str())==4000*1024/100*50);
    assert(BudgetBytes("missing",mode.c_str())==0);
    {std::ofstream f(info);f<<"MemAvailable: 10000 kB\n";}
    assert(BudgetBytes(info.c_str(),mode.c_str())==0);
    budgetPct=0;
    auto dir=std::filesystem::path(root)/"res/config/terrain_generators";std::filesystem::create_directories(dir);
    auto path=dir/"fantasia_map_generator.gen.lua";const std::string src="function data()\n\t\treturn result\nend\n";
    {std::ofstream f(path);f<<src;}
    // An old, sub-second mtime exposes a fresh tmpfile on every open.
    const timespec times[]={{1234567890,123456789},{1234567891,987654321}};
    assert(!utimensat(AT_FDCWD,path.c_str(),times,0));
    auto checkTimes=[&](FILE* f) {
        assert(f);struct stat original{},served{};
        assert(!stat(path.c_str(),&original) && !fstat(fileno(f),&served));
        assert(served.st_mtim.tv_sec==original.st_mtim.tv_sec);
        assert(served.st_mtim.tv_nsec==original.st_mtim.tv_nsec);
        assert(served.st_atim.tv_sec==times[0].tv_sec);
        assert(served.st_atim.tv_nsec==times[0].tv_nsec);
        return f;
    };
    void* code=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(code!=MAP_FAILED);
    memcpy(code,installed,sizeof installed);assert(!mprotect(code,4096,PROT_READ|PROT_EXEC));
    auto routed=reinterpret_cast<FILE*(*)(const char*,const char*)>(code);
    assert(Read(checkTimes(routed(path.c_str(),"rb"))).find("_tpf2_bigmap_memory.Optimize")!=std::string::npos);
    assert(!munmap(code,4096));
    assert(Index("/mods/f/res/config/terrain_generators/fantasia_map_generator_dry.gen.lua")==1);
    assert(Index("/mods/f/res/config/terrain_generators/fantasia_map_generator_tropical.gen.lua")==2);
    assert(Index(path.c_str())==0);assert(Index("/res/scripts/fantasia_map_generator.gen.lua")==-1);
    assert(Index("/res/config/terrain_generators/xfantasia_map_generator.gen.lua")==-1);
    assert(!utimensat(AT_FDCWD,path.c_str(),times,0));
    const auto patched=Read(checkTimes(Open(path.c_str(),"rb")));assert(patched.find("_tpf2_bigmap_memory.Optimize")!=std::string::npos);
    assert(patched.find("_tpf2_bigmap_budget = 0\n")!=std::string::npos);
    assert(Read(std::fopen(path.c_str(),"rb"))==src);
    // Concurrent lifetimes: opening another stream cannot replace the first stream.
    FILE* first=Open(path.c_str(),"rb");assert(Read(Open(path.c_str(),"rb"))==patched);assert(Read(first)==patched);
    assert(Read(Open(path.c_str(),"r+"))==src);
    {std::ofstream f(path);f<<"changed without anchor\n";}
    assert(Read(Open(path.c_str(),"rb"))=="changed without anchor\n");
    std::filesystem::remove_all(root);errno=0;assert(!Open(path.c_str(),"rb") && errno==ENOENT);
}
